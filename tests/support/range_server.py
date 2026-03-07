import argparse
import os
import threading
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class RangeRequestHandler(BaseHTTPRequestHandler):
    server_version = "AsyncDownloadTestServer/1.0"

    def do_HEAD(self):
        self._handle_request(send_body=False)

    def do_GET(self):
        self._handle_request(send_body=True)

    def log_message(self, format, *args):
        return

    def _handle_request(self, send_body: bool):
        file_path = self.server.file_path
        file_size = os.path.getsize(file_path)
        start = 0
        end = file_size - 1
        status = HTTPStatus.OK
        range_header = ""

        request_range = self.headers.get("Range")
        if request_range:
            range_header = request_range
            unit, _, value = request_range.partition("=")
            if unit.strip() != "bytes":
                self.send_error(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                return

            begin_value, _, end_value = value.partition("-")
            if begin_value:
                start = int(begin_value)
            if end_value:
                end = int(end_value)
            if start > end or start >= file_size:
                self.send_error(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                return
            end = min(end, file_size - 1)
            status = HTTPStatus.PARTIAL_CONTENT

        self.server.record_request(self.command, self.client_address[1], range_header)

        content_length = end - start + 1
        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(content_length))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", f'"{self.server.etag}"')
        self.send_header("Last-Modified", self.date_time_string(self.server.last_modified))
        if status == HTTPStatus.PARTIAL_CONTENT:
            self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
        self.end_headers()

        if not send_body:
            return

        with open(file_path, "rb") as stream:
            stream.seek(start)
            remaining = content_length
            while remaining > 0:
                chunk = stream.read(min(self.server.chunk_size, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                self.wfile.flush()
                remaining -= len(chunk)
                if self.server.delay_ms > 0:
                    time.sleep(self.server.delay_ms / 1000.0)


class RangeServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, server_address, file_path, chunk_size, delay_ms, request_log_path):
        super().__init__(server_address, RangeRequestHandler)
        self.file_path = file_path
        self.chunk_size = chunk_size
        self.delay_ms = delay_ms
        self.request_log_path = request_log_path
        self.request_log_lock = threading.Lock()
        stat = os.stat(file_path)
        self.last_modified = stat.st_mtime
        self.etag = f"{stat.st_size:x}-{int(stat.st_mtime):x}"

    def record_request(self, method, client_port, range_header):
        if not self.request_log_path:
            return

        with self.request_log_lock:
            with open(self.request_log_path, "a", encoding="utf-8") as stream:
                stream.write(f"{method} {client_port} {range_header}\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file_path")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--chunk-size", type=int, default=65536)
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--request-log", default="")
    args = parser.parse_args()

    server = RangeServer((args.host, args.port), os.path.abspath(args.file_path), args.chunk_size,
        args.delay_ms, args.request_log)
    actual_port = server.server_address[1]
    print(actual_port, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
