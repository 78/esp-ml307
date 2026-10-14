# Changelog

## 3.7.6 — 2026-10-15

- Require uart-uhci `^0.4.1` on supported targets to include compatibility with the replacement GDMA alignment API in ESP-IDF maintenance branches.
- Include the EspTcp teardown fix for a use-after-free race after passive disconnect.

## 3.7.5

- Require uart-uhci `^0.4.0` on supported targets, allowing use alongside uart-eth-modem 0.7.x.
- Defer dropped and empty RX buffer returns from the ISR to the existing receive task. Use its task notification to wake for reclamation even when no data was queued, without adding buffers or synchronization objects.
- Stop DMA reception before tearing down RX consumers and reclaim deferred buffers during cleanup.
- Size the bounded UHCI transmit timeout from the actual UART baud rate and payload length (8N1 wire time plus 1000 ms). Preserve successful empty sends and propagate transport failures through the existing `AtResult` API.
- Add host regression coverage for RX queue exhaustion, empty delivery, startup/wakeup timing, and transmit deadlines/errors.
