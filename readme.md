# Visrodeck Digital Zones — Medical History Kiosk (local backend)

FastAPI server + web dashboard + printable 80 mm slip. Runs entirely on your laptop.

## Files
Keep these together in one folder: `server.py`, `index.html`, `requirements.txt`.

## Quick start (recommended)
Put `start.py`, `server.py`, `index.html` and `requirements.txt` in one folder, then run:
```bash
python start.py
```
It checks Python, creates `.venv`, installs dependencies, offers to install PaddleOCR, installs `llama-cpp-python` for direct local GGUF inference, picks a free port, prints your laptop's Wi-Fi URL and opens the dashboard. OCR and model inference fall back gracefully if dependencies are unavailable.

Options: `--with-ocr`, `--skip-ocr`, `--skip-model`, `--model bartowski/Llama-3.2-3B-Instruct-GGUF`, `--model-file Llama-3.2-3B-Instruct-Q4_K_M.gguf`, `--port 8080`, `--no-browser`.

The sections below are the manual steps `start.py` automates.

## 1. Install
```bash
python -m venv .venv
# Windows:  .venv\Scripts\activate
# macOS/Linux:  source .venv/bin/activate
pip install -r requirements.txt
```

Optional, for real OCR (the server works without it, using a mock record):
```bash
pip install paddlepaddle
pip install paddleocr
```

For direct local inference, the launcher installs `llama-cpp-python`. By default it downloads the public community GGUF weights (about 2 GB) on the first prescription upload. The default is Q4_K_M quantization. To use a GGUF you already downloaded, set `LLM_MODEL_PATH` to its full path in `.env`. No Ollama service or Hugging Face token is used. The model remains subject to Meta's Llama license. Choose another GGUF repository with `LLM_MODEL_REPO` in `.env`, or pass it with `--model`; set its filename with `LLM_MODEL_FILE` or `--model-file`.

## 2. Start the server
```bash
uvicorn server:app --host 0.0.0.0 --port 8000
```
`--host 0.0.0.0` is what lets other devices on your Wi-Fi reach it. On startup the console prints the Local and Network URLs.

## 3. Find your laptop's local IP
- **Windows:** `ipconfig` → "IPv4 Address" under your Wi-Fi adapter (e.g. 192.168.1.23)
- **macOS:** `ipconfig getifaddr en0`
- **Linux:** `hostname -I` (first address) or `ip addr`
- Or call `GET /api/health` and read `lan_ip`.

## ESP32 Wi-Fi credentials
The Arduino sketch cannot read the laptop's `.env` at runtime. Before compiling,
put the kiosk network name and password in the project-root `.env` file, then run:
```powershell
python tools/generate_wifi_secrets.py
```
This generates `visrodeck_kiosk/wifi_secrets.h` for the sketch. Both `.env` and
the generated header are ignored by Git so credentials stay local. Start with `.env.example`
as a reference; do not upload the `.env` file to the ESP32.

Open `http://<that-ip>:8000` from a phone or the ESP32's network. If it doesn't connect, allow Python through your firewall (Windows: allow private networks on first prompt) and make sure both devices are on the same Wi-Fi.

## API
| Method | Path | Purpose |
|---|---|---|
| GET | `/api/health` | server, PaddleOCR and local llama.cpp model status, LAN IP |
| GET | `/api/patient/{patient_id}/records` | structured history (newest first) |
| POST | `/api/upload-prescription` | multipart: `file` image, `patient_id` (default `Bedu`) |
| POST | `/api/print-slip` | JSON `{"patient_id":"Bedu","record_ids":[1,2]}` (ids optional, defaults to latest); logs the print and returns slip data + `view_url` |
| GET | `/slip/{patient_id}?ids=1,2&print=1` | 80 mm printable slip view |

Quick test:
```bash
curl http://localhost:8000/api/health
curl http://localhost:8000/api/patient/Bedu/records
curl -F "file=@prescription.jpg" -F "patient_id=Bedu" http://localhost:8000/api/upload-prescription
curl -X POST http://localhost:8000/api/print-slip -H "Content-Type: application/json" -d '{"patient_id":"Bedu"}'
```

## Fallback behaviour
1. PaddleOCR missing → a clearly labelled mock record is saved (source `mock`).
2. Model dependencies, model access, or model download unavailable → regex parser on OCR text (source `ocr+fallback`).
3. Local GGUF model available through llama.cpp → strict-JSON extraction (source `ocr+llm`).

Seed history for "Bedu" (3 records) is created on first run. Data lives in `kiosk.db`; uploaded images in `uploads/`. Delete `kiosk.db` to reset.
