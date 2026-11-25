#!/usr/bin/env python3
"""
Visrodeck Digital Zones — Medical History Kiosk: one-command launcher.

    python start.py                 # set everything up and start the server
    python start.py --with-ocr      # also install PaddleOCR without asking
    python start.py --skip-ocr      # never install PaddleOCR (mock/fallback parsing)
    python start.py --skip-model    # don't install/use the local language model
    python start.py --port 8080 --model bartowski/Llama-3.2-3B-Instruct-GGUF

Uses only the Python standard library, so it runs before anything is installed.
If OCR or the local llama.cpp model can't be set up, the server still starts
and uses its seed data + fallback parser.
"""
import argparse
import hashlib
import importlib.util
import os
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.request
import venv
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent
VENV = ROOT / ".venv"
REQ = ROOT / "requirements.txt"
DEPS_MARK = VENV / ".deps_hash"
OCR_DECLINED = VENV / ".ocr_declined"
DEFAULT_MODEL = "bartowski/Llama-3.2-3B-Instruct-GGUF"
DEFAULT_MODEL_FILE = "Llama-3.2-3B-Instruct-Q4_K_M.gguf"
BANNER = "Visrodeck Digital Zones — Medical History Kiosk"


def load_project_env():
    """Load simple KEY=VALUE entries from .env without an extra dependency."""
    env_file = ROOT / ".env"
    if not env_file.is_file():
        return
    for raw in env_file.read_text(encoding="utf-8-sig").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key, value = key.strip(), value.strip()
        if value[:1] == value[-1:] and value[:1] in ("'", '"'):
            value = value[1:-1]
        if key:
            os.environ.setdefault(key, value)


def say(msg=""):
    print(msg, flush=True)


def step(msg):
    say(f"\n▶ {msg}")


def ok(msg):
    say(f"  ✓ {msg}")


def warn(msg):
    say(f"  ! {msg}")


# ------------------------------------------------------------ environment / venv
def venv_python() -> Path:
    """Locate the venv interpreter. Layouts differ: python.org Windows uses Scripts/,
    MSYS2/MinGW Python (and Linux/macOS) use bin/."""
    candidates = [
        VENV / "Scripts" / "python.exe",
        VENV / "bin" / "python.exe",
        VENV / "bin" / "python",
        VENV / "bin" / "python3",
    ]
    for c in candidates:
        if c.exists():
            return c
    return candidates[0]


def inside_venv() -> bool:
    try:
        return Path(sys.prefix).resolve() == VENV.resolve()
    except OSError:
        return False


def check_python():
    if sys.version_info < (3, 9):
        sys.exit(f"Python 3.9+ is required (found {sys.version.split()[0]}). Please upgrade Python.")
    ok(f"Python {sys.version.split()[0]}")
    import sysconfig
    if sysconfig.get_platform().startswith("mingw") or "GCC" in sys.version:
        warn("This looks like an MSYS2/MinGW Python. Some packages (pydantic, PaddleOCR) "
             "often have no prebuilt wheels for it and may fail to install.")
        warn("If setup fails, install Python 3.12 from https://www.python.org/downloads/ "
             "(tick 'Add to PATH') and run:  py -3.12 start.py")
    elif sys.version_info >= (3, 13):
        warn("Python 3.13+ is very new; PaddleOCR may not support it yet. "
             "Python 3.10–3.12 is the safest choice.")


def check_files():
    missing = [f for f in ("server.py", "index.html", "requirements.txt") if not (ROOT / f).exists()]
    if missing:
        sys.exit(f"Missing files next to start.py: {', '.join(missing)}")
    ok("server.py, index.html, requirements.txt found")


def ensure_venv_and_reexec():
    """Create .venv if needed and re-run this script inside it."""
    if inside_venv():
        return
    if not venv_python().exists():
        step("Creating virtual environment (.venv)")
        venv.EnvBuilder(with_pip=True).create(VENV)
        ok("created")
    if not venv_python().exists():
        shutil.rmtree(VENV, ignore_errors=True)  # don't leave a broken venv behind
        sys.exit("The virtual environment was created but has no Python executable inside it.\n"
                 "Install Python 3.12 from https://www.python.org/downloads/ and run:  py -3.12 start.py")
    say("  Restarting inside the virtual environment…")
    code = subprocess.call([str(venv_python()), str(Path(__file__).resolve()), *sys.argv[1:]])
    sys.exit(code)


def pip(*args) -> int:
    return subprocess.call([sys.executable, "-m", "pip", "--disable-pip-version-check", *args])


def install_requirements():
    step("Checking Python dependencies")
    digest = hashlib.sha256(REQ.read_bytes()).hexdigest()
    have = DEPS_MARK.read_text().strip() if DEPS_MARK.exists() else ""
    importable = all(importlib.util.find_spec(m) for m in ("fastapi", "uvicorn", "multipart", "requests"))
    if have == digest and importable:
        ok("already installed")
        return
    say("  Installing from requirements.txt (first run can take a minute)…")
    if pip("install", "-q", "-r", str(REQ)) != 0:
        sys.exit("Could not install requirements. Check your internet connection and try again.")
    DEPS_MARK.write_text(digest)
    ok("installed")


# ------------------------------------------------------------------------- OCR
def ocr_installed() -> bool:
    return importlib.util.find_spec("paddleocr") is not None


def setup_ocr(args):
    step("PaddleOCR (reads prescription images)")
    if ocr_installed():
        ok("installed")
        return
    if args.skip_ocr or OCR_DECLINED.exists() and not args.with_ocr:
        warn("not installed — uploads will use the mock parser (run with --with-ocr to install)")
        return
    want = args.with_ocr
    if not want and sys.stdin.isatty():
        ans = input("  Not installed. Install PaddleOCR now? It is a large download (several hundred MB). [y/N] ")
        want = ans.strip().lower() in ("y", "yes")
        if not want:
            OCR_DECLINED.write_text("declined")
    if not want:
        warn("skipped — uploads will use the mock parser (run with --with-ocr to install later)")
        return
    say("  Installing PaddlePaddle + PaddleOCR… this can take several minutes.")
    if pip("install", "paddlepaddle") != 0 or pip("install", "paddleocr") != 0:
        warn("PaddleOCR install failed — continuing with the mock parser. "
             "(Paddle may not support your Python version yet; Python 3.10–3.12 is safest.)")
        return
    OCR_DECLINED.unlink(missing_ok=True)
    ok("installed")


# ----------------------------------------------------------- direct local model
def setup_local_model(args):
    step(f"Community GGUF model ({args.model}/{args.model_file})")
    if args.skip_model:
        warn("skipped — uploads use the regex fallback parser")
        return
    if importlib.util.find_spec("llama_cpp"):
        ok("llama-cpp-python already installed")
    else:
        say("  Installing the prebuilt CPU llama.cpp Python wheel…")
        if pip("install", "llama-cpp-python", "--extra-index-url",
               "https://abetlen.github.io/llama-cpp-python/whl/cpu",
               "--only-binary=llama-cpp-python") != 0:
            warn("model dependencies failed to install — uploads will use the regex fallback parser")
            return
        ok("llama-cpp-python installed")
    local_model = os.getenv("LLM_MODEL_PATH", "").strip()
    if local_model and Path(local_model).is_file():
        say(f"  Using local GGUF file: {local_model}")
    else:
        say("  Public community GGUF weights (~2 GB) download on the first prescription upload.")


# --------------------------------------------------------------------- network
def lan_ip() -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def port_free(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("0.0.0.0", port))
            return True
        except OSError:
            return False


def pick_port(preferred: int) -> int:
    for p in range(preferred, preferred + 20):
        if port_free(p):
            if p != preferred:
                warn(f"port {preferred} is busy — using {p} instead")
            return p
    sys.exit(f"No free port found near {preferred}. Use --port to choose another.")


def open_browser_when_ready(port: int):
    url = f"http://localhost:{port}"
    for _ in range(60):
        try:
            urllib.request.urlopen(f"{url}/api/health", timeout=1).read()
            webbrowser.open(url)
            return
        except Exception:
            time.sleep(0.5)


# ------------------------------------------------------------------------- main
def main():
    load_project_env()
    ap = argparse.ArgumentParser(description=BANNER)
    ap.add_argument("--port", type=int, default=int(os.getenv("PORT", "8000")))
    ap.add_argument("--model", default=os.getenv("LLM_MODEL_REPO", DEFAULT_MODEL), help="Hugging Face GGUF repository")
    ap.add_argument("--model-file", default=os.getenv("LLM_MODEL_FILE", DEFAULT_MODEL_FILE), help="GGUF filename in the repository")
    ap.add_argument("--with-ocr", action="store_true", help="install PaddleOCR without asking")
    ap.add_argument("--skip-ocr", action="store_true", help="never install PaddleOCR")
    ap.add_argument("--skip-model", action="store_true", help="skip local model dependency setup")
    ap.add_argument("--no-browser", action="store_true", help="don't open a browser tab")
    args = ap.parse_args()

    say(f"\n=== {BANNER} ===")
    step("Checking environment")
    check_python()
    check_files()
    ensure_venv_and_reexec()          # does not return when it has to re-launch
    install_requirements()
    setup_ocr(args)
    setup_local_model(args)

    step("Starting server")
    port = pick_port(args.port)
    env = dict(os.environ, PORT=str(port), LLM_MODEL_REPO=args.model, LLM_MODEL_FILE=args.model_file)
    ip = lan_ip()
    say(f"\n  This laptop:        http://localhost:{port}")
    say(f"  Other devices (Wi-Fi): http://{ip}:{port}")
    say("  Press Ctrl+C to stop.")
    if os.name == "nt":
        say("  (If other devices can't connect, allow Python through Windows Firewall on private networks.)")
    say("")

    if not args.no_browser:
        threading.Thread(target=open_browser_when_ready, args=(port,), daemon=True).start()
    try:
        subprocess.call(
            [sys.executable, "-m", "uvicorn", "server:app", "--host", "0.0.0.0", "--port", str(port)],
            cwd=ROOT, env=env,
        )
    except KeyboardInterrupt:
        pass
    finally:
        say("\nServer stopped.")


if __name__ == "__main__":
    main()
