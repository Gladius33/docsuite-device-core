#!/usr/bin/env python3
import base64
import http.server
import socketserver
import sys
import threading

HOST = "127.0.0.1"
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18080

JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAADAAQDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD9CNT/AGZPg9rV19q1H4UeB7+52LH5114cs5H2qAqrloycAAADsABRRRR0S7afJaL7gP/Z"
)

CAPABILITIES = b'''<?xml version="1.0" encoding="UTF-8"?>
<scan:ScannerCapabilities xmlns:scan="http://schemas.hp.com/imaging/escl/2011/05/03" xmlns:pwg="http://www.pwg.org/schemas/2010/12/sm">
  <pwg:MakeAndModel>DocSuite Mock Scanner 5300</pwg:MakeAndModel>
  <pwg:Manufacturer>DocSuite</pwg:Manufacturer>
  <scan:Platen>
    <scan:PlatenInputCaps>
      <scan:MinWidth>1</scan:MinWidth><scan:MaxWidth>2550</scan:MaxWidth>
      <scan:MinHeight>1</scan:MinHeight><scan:MaxHeight>3508</scan:MaxHeight>
      <scan:SettingProfiles>
        <scan:SettingProfile>
          <scan:ColorModes>
            <scan:ColorMode>RGB24</scan:ColorMode>
            <scan:ColorMode>Grayscale8</scan:ColorMode>
          </scan:ColorModes>
          <scan:SupportedResolutions>
            <scan:DiscreteResolutions>
              <scan:DiscreteResolution><scan:XResolution>150</scan:XResolution><scan:YResolution>150</scan:YResolution></scan:DiscreteResolution>
              <scan:DiscreteResolution><scan:XResolution>300</scan:XResolution><scan:YResolution>300</scan:YResolution></scan:DiscreteResolution>
              <scan:DiscreteResolution><scan:XResolution>600</scan:XResolution><scan:YResolution>600</scan:YResolution></scan:DiscreteResolution>
            </scan:DiscreteResolutions>
          </scan:SupportedResolutions>
        </scan:SettingProfile>
      </scan:SettingProfiles>
    </scan:PlatenInputCaps>
  </scan:Platen>
</scan:ScannerCapabilities>'''


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    next_document_attempts = 0
    attempt_lock = threading.Lock()

    def log_message(self, fmt, *args):
        pass

    def send_bytes(self, status, data, content_type, extra_headers=None):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        for key, value in (extra_headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if data:
            self.wfile.write(data)

    def do_GET(self):
        if self.path == "/eSCL/ScannerCapabilities":
            self.send_bytes(200, CAPABILITIES, "text/xml")
            return

        if self.path == "/eSCL/ScanJobs/1/NextDocument":
            with type(self).attempt_lock:
                type(self).next_document_attempts += 1
                attempt = type(self).next_document_attempts

            if attempt <= 2:
                self.send_bytes(
                    503,
                    b"scanner warming up",
                    "text/plain",
                    {"Retry-After": "1"},
                )
                return

            self.send_bytes(200, JPEG, "image/jpeg")
            return

        if self.path == "/eSCL/ScannerStatus":
            status = b'<scan:ScannerStatus xmlns:scan="http://schemas.hp.com/imaging/escl/2011/05/03" xmlns:pwg="http://www.pwg.org/schemas/2010/12/sm"><pwg:State>Idle</pwg:State></scan:ScannerStatus>'
            self.send_bytes(200, status, "text/xml")
            return

        self.send_bytes(404, b"", "text/plain")

    def do_POST(self):
        if self.path != "/eSCL/ScanJobs":
            self.send_bytes(404, b"", "text/plain")
            return

        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        required = [
            b"<scan:ColorMode>RGB24</scan:ColorMode>",
            b"<scan:XResolution>300</scan:XResolution>",
            b"<scan:YResolution>300</scan:YResolution>",
            b"<pwg:InputSource>Platen</pwg:InputSource>",
            b"<scan:ScanRegions>",
            b"<pwg:Width>2550</pwg:Width>",
            b"<pwg:Height>3508</pwg:Height>",
            b"<pwg:XOffset>0</pwg:XOffset>",
            b"<pwg:YOffset>0</pwg:YOffset>",
        ]
        missing = [token.decode("ascii") for token in required if token not in body]
        if missing:
            message = ("invalid ScanSettings; missing: " + ", ".join(missing)).encode()
            self.send_bytes(400, message, "text/plain")
            return

        self.send_response(201)
        self.send_header("Location", f"http://{HOST}:{PORT}/eSCL/ScanJobs/1")
        self.send_header("Content-Length", "0")
        self.end_headers()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


if __name__ == "__main__":
    with Server((HOST, PORT), Handler) as server:
        server.serve_forever()
