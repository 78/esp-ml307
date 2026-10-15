# Changelog

## 3.7.7 — 2026-10-16

Fixes in the built-in HTTP/1.1 client (`HttpClient`, used by the ESP and EC801E backends):

- Fail `GetStatusCode()` and `Read()` with `ServerDisconnected` as soon as the connection closes before the response headers arrive, instead of waiting for the full timeout.
- Complete HEAD, 1xx, 204, 304 and `Content-Length: 0` responses at the end of the headers, so `Read()` no longer waits for the timeout on keep-alive connections.
- Reject malformed chunk sizes, invalid `Content-Length`, non-HTTP status lines and lines over 8 KB with `ProtocolError`. A bad chunk size was previously treated as the last chunk and silently truncated the body. Parse errors now wake readers and prevent connection reuse.
- Compare keep-alive reuse against the endpoint of the open connection. The new URL was previously compared with itself, so a request to another host could reuse the old connection. Reuse also requires a fully parsed response and keep-alive enabled locally.
- Join the transport's receive task before tearing down client state in the destructor and when `Open()` replaces a connection, so late callbacks cannot touch freed or reset state.
- Avoid lost reader wake-ups by publishing state changes through the reader mutex; shared flags are atomic.
- Match `Transfer-Encoding: chunked` case-insensitively, skip interim 1xx responses, drop bytes beyond `Content-Length`, and return 0 from `Read()` with an empty buffer instead of discarding queued data.
- Build requests without `std::ostringstream` to avoid linking iostream/locale code. Translate source comments to English.
- Add host regression tests for `HttpClient` to `tests/run_host_tests.py`.

## 3.7.6 — 2026-10-15

- Require uart-uhci `^0.4.1` on supported targets to include compatibility with the replacement GDMA alignment API in ESP-IDF maintenance branches.
- Include the EspTcp teardown fix for a use-after-free race after passive disconnect.

## 3.7.5

- Require uart-uhci `^0.4.0` on supported targets, allowing use alongside uart-eth-modem 0.7.x.
- Defer dropped and empty RX buffer returns from the ISR to the existing receive task. Use its task notification to wake for reclamation even when no data was queued, without adding buffers or synchronization objects.
- Stop DMA reception before tearing down RX consumers and reclaim deferred buffers during cleanup.
- Size the bounded UHCI transmit timeout from the actual UART baud rate and payload length (8N1 wire time plus 1000 ms). Preserve successful empty sends and propagate transport failures through the existing `AtResult` API.
- Add host regression coverage for RX queue exhaustion, empty delivery, startup/wakeup timing, and transmit deadlines/errors.
