import http.server
import os

PORT = 8080
DIR = os.path.dirname(os.path.abspath(__file__))

os.chdir(DIR)

class OTAHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.lstrip('/')
        if not path or path == '/':
            path = 'ota.bin'
        if not os.path.exists(path):
            self.send_error(404)
            return
        size = os.path.getsize(path)
        self.send_response(200)
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Content-Length', str(size))
        self.end_headers()
        with open(path, 'rb') as f:
            while True:
                chunk = f.read(4096)
                if not chunk:
                    break
                self.wfile.write(chunk)

    def log_message(self, format, *args):
        print(f"[OTA] {args[0]}")

print(f'OTA file server running on http://0.0.0.0:{PORT}')
print(f'Serving: {DIR}/ota.bin')
print(f'OTA URL: http://<YOUR_IP>:{PORT}/ota.bin')
print('Press Ctrl+C to stop')

http.server.HTTPServer(('0.0.0.0', PORT), OTAHandler).serve_forever()
