# Changelog

## 3.8.1 — 2026-10-17

- Replace C++ string streams in AT argument parsing and ML307 HTTP response header parsing with direct string scans. Applications that do not use streams elsewhere can discard the associated iostream/locale code.
- Preserve empty AT argument positions without accessing the front of an empty string.
- Remove unused `<sstream>` and `<iomanip>` includes from modem backends.

## 3.8.0 — 2026-10-16

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

Transport fixes:

- `EspTcp`: connect in non-blocking mode with a 10 s timeout (same as esp-tls) and set a 10 s `SO_SNDTIMEO`. Blocking `connect()` followed `CONFIG_LWIP_TCP_SYNMAXRTX` and could hang for minutes on `http://` URLs when the data path was down. Based on #51 and #52 by @Vitalymt.
- `EspTcp`: only `Disconnect()` closes the fd, after `shutdown()` has woken the receive task and it has exited. A passive close no longer closes the fd from the receive task, which could race with `Disconnect()` and close a socket that another task had just been given the same number. `Connect()` cleans up after a passive disconnect, and the disconnect callback fires once. `connected_` is set again before the receive task starts, so the task can no longer exit before its first `recv()` (#62, reported with a fix by @vuitv).
- `EspTcp`/`EspUdp`: give the receive socket a 1 s `SO_RCVTIMEO` and re-check `connected_` on timeout. lwIP can lose the wake-up of a `recv()` that is just starting when the socket is shut down or closed; the task then blocked forever, `Disconnect()` waited 10 s and the task outlived the object (#63, reported with a fix by @vuitv). `EspSsl` waits in `select()`, which does not have this race.
- `EspSsl`: switch the socket to non-blocking after the handshake and serialize every mbedTLS read/write with a mutex; waiting happens in `select()` outside the lock. The receive task and `Send()` previously drove the same SSL context concurrently, which is undefined, e.g. for TLS 1.3 NewSessionTicket processing.
- `EspSsl`: fail `Connect()` when the receive task cannot be created, bound `Send()` to 10 s without progress instead of retrying `WANT_WRITE` forever (reported in #52 by @Vitalymt), allow `Connect()` again after a passive disconnect, and stop aborting via `ESP_ERROR_CHECK` in `Disconnect()`.
- `EspTcp`/`EspUdp`: resolve with `getaddrinfo()` instead of the non-reentrant `gethostbyname()`/`inet_ntoa()`.
- `EspUdp`: `Connect()` releases the fd left by a receive error instead of leaking it, check `xTaskCreate()`, and keep receiving after empty datagrams.
- `WebSocket`: reset handshake and frame state on reconnect, destroy the transport first in the destructor and after a failed handshake, and fail the connection on frames or messages larger than `WebSocket::MAX_MESSAGE_SIZE` (256 KB). A 64-bit length could previously grow the buffer without limit or overflow the length check.
- ML307/EC801E: on `FIFO_OVERFLOW`, mark TCP/SSL/UDP/HTTP instances broken without sending AT commands from the AT event task, which used to block that task (and every in-flight AT command) until timeout once per instance. The close command is sent by the next `Disconnect()`/`Connect()`/`Close()`.
- ML307: check URC argument counts before indexing (`MIPURC` in `Ml307Tcp`/`Ml307Udp`, `MHTTPURC`/`MHTTPCREATE` in `Ml307Http`); `Ml307Udp` now handles the 3-argument `disconn` URC. `Ec801ESsl` frees its event group.
- `WebSocket`: add `OnPing()` and `SendPong()`. With an `OnPing` callback set, the library no longer replies to Pings by itself, so callers on AT transports can send the Pong from another task; the automatic reply ran on the AT event task and stalled it until the send timed out. Without a callback the Pong is still sent automatically.
- Add host regression tests for `WebSocket`.

## 3.7.6 — 2026-10-15

- Require uart-uhci `^0.4.1` on supported targets to include compatibility with the replacement GDMA alignment API in ESP-IDF maintenance branches.
- Include the EspTcp teardown fix for a use-after-free race after passive disconnect.

## 3.7.5

- Require uart-uhci `^0.4.0` on supported targets, allowing use alongside uart-eth-modem 0.7.x.
- Defer dropped and empty RX buffer returns from the ISR to the existing receive task. Use its task notification to wake for reclamation even when no data was queued, without adding buffers or synchronization objects.
- Stop DMA reception before tearing down RX consumers and reclaim deferred buffers during cleanup.
- Size the bounded UHCI transmit timeout from the actual UART baud rate and payload length (8N1 wire time plus 1000 ms). Preserve successful empty sends and propagate transport failures through the existing `AtResult` API.
- Add host regression coverage for RX queue exhaustion, empty delivery, startup/wakeup timing, and transmit deadlines/errors.
