import http.server
import json
import os
import hashlib
import hmac
import urllib.request
import zipfile
import io

PORT = int(os.environ.get('OTA_PORT', 8081))
DATA_DIR = '/data' if os.path.exists('/data') else os.path.dirname(os.path.abspath(__file__))
WEBHOOK_SECRET = os.environ.get('WEBHOOK_SECRET', '')

os.makedirs(DATA_DIR, exist_ok=True)

def get_version(branch='dev'):
    version_file = os.path.join(DATA_DIR, branch, 'version.txt')
    if os.path.exists(version_file):
        with open(version_file, 'r') as f:
            return f.read().strip()
    return ''

def set_version(branch, version):
    branch_dir = os.path.join(DATA_DIR, branch)
    os.makedirs(branch_dir, exist_ok=True)
    with open(os.path.join(branch_dir, 'version.txt'), 'w') as f:
        f.write(version)

def get_firmware_path(branch='dev'):
    return os.path.join(DATA_DIR, branch, 'firmware.bin')

def verify_webhook(payload, signature):
    if not WEBHOOK_SECRET:
        return True
    if not signature:
        return False
    expected = 'sha256=' + hmac.new(WEBHOOK_SECRET.encode(), payload, hashlib.sha256).hexdigest()
    return hmac.compare_digest(expected, signature)

def download_firmware(url, branch):
    print(f"[OTA] Downloading firmware from: {url} (branch: {branch})")
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req) as response:
            data = response.read()
            firmware_path = get_firmware_path(branch)
            with open(firmware_path, 'wb') as f:
                f.write(data)
            print(f"[OTA] Firmware saved: {len(data)} bytes -> {firmware_path}")
            return True
    except Exception as e:
        print(f"[OTA] Download failed: {e}")
        return False

def extract_firmware_from_zip(url, branch):
    print(f"[OTA] Downloading ZIP from: {url} (branch: {branch})")
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req) as response:
            zip_data = response.read()
            with zipfile.ZipFile(io.BytesIO(zip_data)) as zf:
                for name in zf.namelist():
                    if name.endswith('.ota.bin') or name.endswith('.bin'):
                        firmware_path = get_firmware_path(branch)
                        with zf.open(name) as src, open(firmware_path, 'wb') as dst:
                            dst.write(src.read())
                        print(f"[OTA] Extracted: {name} -> {firmware_path}")
                        return True
        print("[OTA] No .bin file found in ZIP")
        return False
    except Exception as e:
        print(f"[OTA] ZIP extract failed: {e}")
        return False

def get_branch_from_ref(ref):
    if not ref:
        return 'dev'
    ref = ref.replace('refs/heads/', '')
    if ref == 'master' or ref == 'main':
        return 'master'
    return 'dev'

class OTAHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split('?')[0]
        params = {}
        if '?' in self.path:
            for param in self.path.split('?')[1].split('&'):
                k, v = param.split('=', 1) if '=' in param else (param, '')
                params[k] = v

        branch = params.get('branch', 'dev')

        if path == '/version':
            version = get_version(branch)
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(version.encode())
            return

        if path == '/firmware':
            firmware_path = get_firmware_path(branch)
            if not os.path.exists(firmware_path):
                self.send_error(404, f'No firmware available for branch: {branch}')
                return
            size = os.path.getsize(firmware_path)
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(size))
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            with open(firmware_path, 'rb') as f:
                while True:
                    chunk = f.read(8192)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
            return

        if path == '/branches':
            branches = []
            for b in ['dev', 'master']:
                if os.path.exists(os.path.join(DATA_DIR, b, 'version.txt')):
                    branches.append({'name': b, 'version': get_version(b)})
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(json.dumps(branches).encode())
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
            target = release.get('target_commitish', '')
            branch = get_branch_from_ref(target)
            print(f"[OTA] Release event: {tag} (branch: {branch})")

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
                ok = extract_firmware_from_zip(firmware_url, branch)
            else:
                ok = download_firmware(firmware_url, branch)

            if ok:
                set_version(branch, tag)
                print(f"[OTA] Updated branch {branch} to {tag}")
            else:
                print(f"[OTA] Firmware update failed for branch {branch}")

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
    print(f'OTA Server running on http://0.0.0.0:{PORT}')
    print(f'Data directory: {DATA_DIR}')
    print(f'Branches:')
    for b in ['dev', 'master']:
        print(f'  {b}: {get_version(b) or "(none)"}')
    print(f'Endpoints:')
    print(f'  GET  /version?branch=dev   - Current version')
    print(f'  GET  /firmware?branch=dev  - Download firmware')
    print(f'  GET  /branches             - List branches')
    print(f'  POST /webhook              - GitHub release webhook')
    print(f'Press Ctrl+C to stop')

    http.server.HTTPServer(('0.0.0.0', PORT), OTAHandler).serve_forever()
