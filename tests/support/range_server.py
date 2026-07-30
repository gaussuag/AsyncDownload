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
        content_range = ""
        content_encoding = ""

        request_range = self.headers.get("Range")
        if self.command == "HEAD" and self.server.head_status != HTTPStatus.OK:
            status = HTTPStatus(self.server.head_status)
        elif request_range and not self.server.disable_ranges:
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
            if not self.server.ignore_range_requests:
                status = HTTPStatus.PARTIAL_CONTENT
                content_range = (
                    f"bytes {start + self.server.content_range_start_delta}-{end}/{file_size}"
                )
        elif request_range:
            range_header = request_range

        content_length = end - start + 1
        if self.server.force_content_encoding:
            content_encoding = self.server.force_content_encoding

        omit_content_length = (
            self.command == "HEAD"
            and self.server.omit_head_content_length
        )

        self.send_response(status)
        self.send_header("Content-Type", "application/octet-stream")
        if not omit_content_length:
            self.send_header("Content-Length", str(content_length))
        if not self.server.disable_ranges:
            self.send_header("Accept-Ranges", "bytes")
        self.send_header("ETag", f'"{self.server.etag}"')
        self.send_header("Last-Modified", self.date_time_string(self.server.last_modified))
        if status == HTTPStatus.PARTIAL_CONTENT:
            self.send_header("Content-Range", content_range)
        if content_encoding:
            self.send_header("Content-Encoding", content_encoding)
        self.end_headers()

        body_bytes = content_length if send_body else 0
        self.server.record_request(
            self.command,
            self.request_version,
            self.client_address[1],
            range_header,
            self.headers.get("Accept-Encoding", ""),
            int(status),
            content_range,
            content_encoding,
            body_bytes,
        )

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

    def __init__(
        self,
        server_address,
        file_path,
        chunk_size,
        delay_ms,
        request_log_path,
        disable_ranges,
        ignore_range_requests,
        head_status,
        omit_head_content_length,
        content_range_start_delta,
        force_content_encoding,
    ):
        super().__init__(server_address, RangeRequestHandler)
        self.file_path = file_path
        self.chunk_size = chunk_size
        self.delay_ms = delay_ms
        self.request_log_path = request_log_path
        self.disable_ranges = disable_ranges
        self.ignore_range_requests = ignore_range_requests
        self.head_status = head_status
        self.omit_head_content_length = omit_head_content_length
        self.content_range_start_delta = content_range_start_delta
        self.force_content_encoding = force_content_encoding
        self.request_log_lock = threading.Lock()
        self.request_ordinal = 0
        stat = os.stat(file_path)
        self.last_modified = stat.st_mtime
        self.etag = f"{stat.st_size:x}-{int(stat.st_mtime):x}"

    def record_request(
        self,
        method,
        http_version,
        client_port,
        range_header,
        accept_encoding,
        response_status,
        response_content_range,
        response_content_encoding,
        body_bytes,
    ):
        if not self.request_log_path:
            return

        with self.request_log_lock:
            self.request_ordinal += 1
            with open(self.request_log_path, "a", encoding="utf-8") as stream:
                fields = (
                    self.request_ordinal,
                    method,
                    http_version,
                    client_port,
                    range_header,
                    accept_encoding,
                    response_status,
                    response_content_range,
                    response_content_encoding,
                    body_bytes,
                )
                stream.write("\t".join(str(field) for field in fields) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file_path")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--chunk-size", type=int, default=65536)
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--request-log", default="")
    parser.add_argument("--disable-ranges", action="store_true")
    parser.add_argument("--ignore-range-requests", action="store_true")
    parser.add_argument("--head-status", type=int, default=200)
    parser.add_argument("--omit-head-content-length", action="store_true")
    parser.add_argument("--content-range-start-delta", type=int, default=0)
    parser.add_argument("--force-content-encoding", default="")
    args = parser.parse_args()

    server = RangeServer((args.host, args.port), os.path.abspath(args.file_path), args.chunk_size,
        args.delay_ms, args.request_log, args.disable_ranges, args.ignore_range_requests,
        args.head_status, args.omit_head_content_length, args.content_range_start_delta,
        args.force_content_encoding)
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
