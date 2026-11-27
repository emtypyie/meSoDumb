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

Required for prescription photo uploads (no sample report is saved when OCR is unavailable):
```bash
pip install paddlepaddle
pip install paddleocr
```

For direct local inference, the launcher installs `llama-cpp-python`. The GGUF weights (about 2 GB) load on the first summary request; prescription uploads use OCR plus the lightweight fallback parser until then. To use a GGUF you already downloaded, set `LLM_MODEL_PATH` to its full path in `.env`. No Ollama service or Hugging Face token is used. The model remains subject to Meta's Llama license. Choose another GGUF repository with `LLM_MODEL_REPO` in `.env`, or pass it with `--model`; set its filename with `LLM_MODEL_FILE` or `--model-file`.

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

## Connect the ESP32 through a phone hotspot
Connect the laptop to the phone hotspot first. Find the laptop's hotspot-side IPv4
address with `ipconfig` on Windows, or use the Network URL printed by the server.
Set the phone hotspot to 2.4 GHz if it offers a band setting; classic ESP32 boards
cannot join a 5 GHz-only hotspot.
Set these values in the project-root `.env` file:

```dotenv
WIFI_SSID=your_phone_hotspot_name
WIFI_PASSWORD=your_phone_hotspot_password
KIOSK_SERVER_URL=http://<laptop-hotspot-ip>:8000
```

Keep the server on port 8000 so the URL matches. Start it with:

```powershell
python -m uvicorn server:app --host 0.0.0.0 --port 8000
```

Then generate the ESP32 configuration and compile/upload `visrodeck_kiosk/visrodeck_kiosk.ino`:

```powershell
python tools/generate_wifi_secrets.py
```
This generates `visrodeck_kiosk/wifi_secrets.h` with the hotspot credentials and
server URL. Both `.env` and the generated header are ignored by Git, so credentials
stay local. Start with `.env.example` as a reference; do not upload `.env` itself
to the ESP32. Connect the phone and ESP32 to the same hotspot. The sketch loads
the `Bedu` records from `/api/patient/Bedu/records` and sends print requests to
`/api/print-slip` on the laptop.

## Live prescription demo
1. Open the laptop dashboard at `http://localhost:8000` while the laptop is
   connected to the phone hotspot.
2. Scan the QR shown beside the patient ID with the phone.
3. Take a clear photo and tap **Lock In & Upload**. The laptop scans and saves it.
4. Click **Summarize latest** for just the most recently uploaded report, or
   **Summarize all** to include both demo records and the new upload.
5. On the ESP32, authenticate again or choose **Refresh records** from its menu
   to load the newest saved report. Demo entries are labelled `DEMO` on the kiosk.

Open `http://<laptop-hotspot-ip>:8000` from the phone to check the dashboard. If
the ESP32 cannot connect, allow Python through Windows Firewall on private
networks and check that the hotspot allows devices to communicate with each other.

## API
| Method | Path | Purpose |
|---|---|---|
| GET | `/api/health` | server, PaddleOCR and local llama.cpp model status, LAN IP |
| GET | `/api/upload-link?patient_id=Bedu` | phone upload page link for the current network |
| GET | `/api/upload-qr.svg?patient_id=Bedu` | QR code for the phone upload page |
| GET | `/phone-upload?patient_id=Bedu` | phone camera capture and lock-in page |
| GET | `/api/patient/{patient_id}/records` | structured history (newest first) |
| POST | `/api/patient/{patient_id}/summary?scope=latest` | summarize the newest uploaded or demo report |
| POST | `/api/patient/{patient_id}/summary?scope=all` | summarize all saved reports, clearly labelling demo records |
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

## Recognition behaviour
1. PaddleOCR missing or unable to scan → the upload returns an error; no fake report is saved.
2. Model dependencies, model access, or model download unavailable → regex parser on OCR text (source `ocr+fallback`).
3. Local GGUF model available through llama.cpp → strict-JSON extraction (source `ocr+llm`).

Older built-in `seed` and fake `mock` rows are removed on server startup. Two
clearly labelled demo history records are kept for the Bedu showcase, alongside
any real OCR uploads in `kiosk.db`; uploaded images live in `uploads/`.
