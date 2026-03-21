# External Integrations

**Analysis Date:** 2026-03-21

## APIs & External Services

**External APIs:**
- HTTP/HTTPS origin servers - the CLI and engine download from arbitrary URLs supplied at runtime
  - Integration method: `libcurl` requests in `src/download/http_probe.cpp` and the download engine
  - Auth: none defined in the repository
  - Endpoints used: standard resource URLs plus `HEAD` and Range GET requests
  - Protocol expectations: `Accept-Ranges`, `Content-Length`, `Content-Range`, `ETag`, and `Last-Modified` are consumed when available

## Data Storage

**Databases:**
- None - no database client or connection string configuration exists in the repository

**File Storage:**
- Local filesystem - downloaded payloads are written to `output_path`, with recovery artifacts alongside it
  - Files: `.part` and `.config.json` recovery files described in `README.md` and implemented in `src/metadata/metadata_store.cpp`
  - Client: `std::filesystem`, `std::fstream`, and native file I/O in `src/storage/file_writer.cpp`

**Caching:**
- None - no Redis, memcached, or similar cache integration exists

## Authentication & Identity

**Auth Provider:**
- None - the repository does not define login, tokens, or session management

**OAuth Integrations:**
- None

## Monitoring & Observability

**Error Tracking:**
- None

**Analytics:**
- None

**Logs:**
- Standard output and standard error only from `src/main.cpp`

## CI/CD & Deployment

**Hosting:**
- None defined in the repository

**CI Pipeline:**
- None defined in the repository

## Environment Configuration

**Development:**
- Required env vars: `VCPKG_ROOT`
- External tools: `python` for `tests/support/range_server.py`
- No secrets file or secret manager is referenced

**Staging:**
- None defined

**Production:**
- None defined

## Webhooks & Callbacks

**Incoming:**
- None

**Outgoing:**
- None

## Test-Only Integrations

**Local HTTP test server:**
- `tests/support/range_server.py` - local Python range-capable HTTP server used by integration tests
  - Endpoint: `http://127.0.0.1:<port>/source.bin`
  - Purpose: verify resume, range fan-out, progress reporting, and failure recovery against a controlled origin
  - Verification: started by `tests/download/download_resume_integration_test.cpp` through Win32 process control

---

*Integration audit: 2026-03-21*
*Update when adding/removing external services*
