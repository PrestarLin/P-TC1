import http.server
import json
import os
import hashlib
import hmac
import urllib.request
import zipfile
import io
import shutil

PORT = int(os.environ.get('OTA_PORT', 8081))
DATA_DIR = '/data' if os.path.exists('/data') else os.path.dirname(os.path.abspath(__file__))
VERSION_FILE = os.path.join(DATA_DIR, 'version.txt')
FIRMWARE_FILE = os.path.join(DATA_DIR, 'firmware.bin')
WEBHOOK_SECRET = os.environ.get('WEBHOOK_SECRET', '')

os.makedirs(DATA_DIR, exist_ok=True)

def get_version():
    if os.path.exists(VERSION_FILE):
        with open(VERSION_FILE, 'r') as f:
            return f.read().strip()
    return ''

def set_version(version):
    with open(VERSION_FILE, 'w') as f:
        f.write(version)

def verify_webhook(payload, signature):
    if not WEBHOOK_SECRET:
        return True
    if not signature:
        return False
    expected = 'sha256=' + hmac.new(WEBHOOK_SECRET.encode(), payload, hashlib.sha256).hexdigest()
    return hmac.compare_digest(expected, signature)

def download_firmware(url):
    print(f"[OTA] Downloading firmware from: {url}")
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req) as response:
            data = response.read()
            with open(FIRMWARE_FILE, 'wb') as f:
                f.write(data)
            print(f"[OTA] Firmware saved: {len(data)} bytes")
            return True
    except Exception as e:
        print(f"[OTA] Download failed: {e}")
        return False

def extract_firmware_from_zip(url):
    print(f"[OTA] Downloading ZIP from: {url}")
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req) as response:
            zip_data = response.read()
            with zipfile.ZipFile(io.BytesIO(zip_data)) as zf:
                for name in zf.namelist():
                    if name.endswith('.ota.bin') or name.endswith('.bin'):
                        with zf.open(name) as src, open(FIRMWARE_FILE, 'wb') as dst:
                            dst.write(src.read())
                        print(f"[OTA] Extracted: {name}")
                        return True
        print("[OTA] No .bin file found in ZIP")
        return False
    except Exception as e:
        print(f"[OTA] ZIP extract failed: {e}")
        return False

class OTAHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split('?')[0]

        if path == '/version':
            version = get_version()
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(version.encode())
            return

        if path == '/firmware':
            if not os.path.exists(FIRMWARE_FILE):
                self.send_error(404, 'No firmware available')
                return
            size = os.path.getsize(FIRMWARE_FILE)
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(size))
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            with open(FIRMWARE_FILE, 'rb') as f:
                while True:
                    chunk = f.read(8192)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            return

        self.send_error(404)

    def do_POST(self):
        path = self.path.split('?')[0]

        if path == '/webhook':
            content_length = int(self.headers.get('Content-Length', 0))
            payload = self.rfile.read(content_length)
            signature = self.headers.get('X-Hub-Signature-256', '')

            if not verify_webhook(payload, signature):
                self.send_error(403, 'Invalid signature')
                return

            try:
                event = json.loads(payload)
            except:
                self.send_error(400, 'Invalid JSON')
                return

            if 'release' not in event:
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'OK')
                return

            release = event['release']
            tag = release.get('tag_name', '')
            print(f"[OTA] Release event: {tag}")

            firmware_url = None
            for asset in release.get('assets', []):
                name = asset.get('name', '')
                if name.endswith('.ota.bin'):
                    firmware_url = asset.get('browser_download_url')
                    break
                if name.endswith('.zip'):
                    firmware_url = asset.get('browser_download_url')
                    break

            if not firmware_url:
                print("[OTA] No firmware asset found in release")
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'OK')
                return

            if firmware_url.endswith('.zip'):
                ok = extract_firmware_from_zip(firmware_url)
            else:
                ok = download_firmware(firmware_url)

            if ok:
                set_version(tag)
                print(f"[OTA] Updated to {tag}")
            else:
                print("[OTA] Firmware update failed")

            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'OK')
            return

        self.send_error(404)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, OPTIONS')
        self.send_header('Access-Control-Allow-Headers', 'Content-Type, X-Hub-Signature-256')
        self.end_headers()

    def log_message(self, format, *args):
        print(f"[OTA] {args[0]}")

if __name__ == '__main__':
    version = get_version()
    print(f'OTA Server running on http://0.0.0.0:{PORT}')
    print(f'Data directory: {DATA_DIR}')
    print(f'Current version: {version or "(none)"}')
    print(f'Endpoints:')
    print(f'  GET  /version   - Current version')
    print(f'  GET  /firmware  - Download firmware')
    print(f'  POST /webhook   - GitHub release webhook')
    print(f'Press Ctrl+C to stop')

    http.server.HTTPServer(('0.0.0.0', PORT), OTAHandler).serve_forever()
