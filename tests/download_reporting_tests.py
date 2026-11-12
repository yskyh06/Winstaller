"""Isolated HTTP redirect/range/error and TLS rejection tests; no external downloads."""
import hashlib
import http.server
import json
import pathlib
import ssl
import subprocess
import sys
import tempfile
import threading

probe = pathlib.Path(sys.argv[1]).resolve()
data = b'litewin-test-payload-' * 5000
sha = hashlib.sha256(data).hexdigest()

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def do_GET(self):
        if self.path.startswith('/redirect'):
            self.send_response(302)
            self.send_header('Location', '/payload?private=secret-token')
            self.end_headers()
            return
        if self.path.startswith('/failure'):
            self.send_response(403); self.end_headers(); return
        start = int(self.headers.get('Range', 'bytes=0-').split('=')[1].split('-')[0])
        content = data[start:]
        self.send_response(206 if start else 200)
        self.send_header('Content-Length', str(len(content)))
        self.send_header('Content-Type', 'application/octet-stream')
        if start: self.send_header('Content-Range', f'bytes {start}-{len(data)-1}/{len(data)}')
        self.end_headers()
        self.wfile.write(content)

def serve(server):
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return thread

with tempfile.TemporaryDirectory(prefix='litewin-http-') as tmp:
    root = pathlib.Path(tmp)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    serve(server)
    def run(path, expected=sha, url=None):
        url = url or f'http://127.0.0.1:{server.server_port}/{path}'
        out = subprocess.check_output([str(probe), '--probe', url, str(root/'out.bin'), expected], text=True, timeout=20)
        assert 'secret-token' not in out and 'user:password' not in out
        return json.loads(out)
    try:
        r = run('redirect')
        assert r['success'] and r['hash'] == 3 and r['transport'] == 2, r
        assert r['host'] == '127.0.0.1' and r['bytes'] == len(data), r
        (root/'out.bin.part').write_bytes(data[:1000])
        r = run('payload')
        assert r['success'] and r['resume'] == 1000 and r['bytes'] == len(data), r
        assert (root/'out.bin').read_bytes() == data
        r = run('payload', '0'*64)
        assert not r['success'] and r['hash'] == 4 and r['phase'] == 4, r
        r = run('failure', '')
        assert not r['success'] and not r['verified_seen'], r
        r = run('payload', '')
        assert r['success'] and r['hash'] == 0 and not r['verified_seen'], r
        cert, key = root/'cert.pem', root/'key.pem'
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                        '-subj', '/CN=localhost', '-keyout', str(key), '-out', str(cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        tls = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        tls.socket = context.wrap_socket(tls.socket, server_side=True)
        serve(tls)
        try:
            r = run('', '', url=f'https://127.0.0.1:{tls.server_port}/payload')
            assert not r['success'] and r['transport'] == 4 and not r['verified_seen'], r
        finally:
            tls.shutdown(); tls.server_close()
    finally:
        server.shutdown(); server.server_close()
print('HTTP redirect, resume, hash, source redaction and TLS rejection passed.')
