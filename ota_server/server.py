import http.server
import os
import urllib.parse

# 遗留的极简手动分发脚本，正式方案是 ../ota-server/server.py（带 webhook、校验与版本目录）。
# self.path 会直接拼成磁盘路径，必须限制在 DIR 内，否则 /../../etc/passwd 可读任意文件。
PORT = 8080
DIR = os.path.dirname(os.path.abspath(__file__))

os.chdir(DIR)

class OTAHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        name = urllib.parse.unquote(self.path).lstrip('/')
        if not name or name == '/':
            name = 'ota.bin'
        path = os.path.realpath(os.path.join(DIR, name))
        if not path.startswith(DIR + os.sep) or not os.path.isfile(path):
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
