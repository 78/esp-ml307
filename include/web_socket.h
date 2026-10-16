#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <atomic>
#include <functional>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "network_error.h"
#include "tcp.h"

class NetworkInterface;

class WebSocket {
public:
    // Largest frame payload or reassembled message accepted from the peer.
    // Larger frames fail the connection (OnError, then OnDisconnected).
    static constexpr size_t MAX_MESSAGE_SIZE = 256 * 1024;

    WebSocket(NetworkInterface* network, int connect_id);
    ~WebSocket();

    void SetHeader(const char* key, const char* value);
    void SetReceiveBufferSize(size_t size);
    bool IsConnected() const;
    NetworkResult<> Connect(const char* uri);
    bool Send(const std::string& data);
    bool Send(const void* data, size_t len, bool binary = false, bool fin = true);
    void Ping();
    // Sends a Pong control frame; payload must be at most 125 bytes.
    bool SendPong(const void* data, size_t len);
    void Close();

    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);
    void OnData(std::function<void(const char*, size_t, bool binary)> callback);
    void OnError(std::function<void(const NetworkError& error)> callback);
    // Invoked when a Pong control frame (opcode 0xA) is received from the
    // peer, carrying the Pong's application data payload (which echoes the
    // payload of the Ping that triggered it). Lets callers implement an
    // active liveness check (send Ping(), expect OnPong within a timeout)
    // or measure round-trip time, without the library imposing any timeout
    // policy of its own.
    void OnPong(std::function<void(const char*, size_t)> callback);
    // Invoked when a Ping control frame (opcode 0x9) is received, carrying its
    // application data payload. Once set, the library no longer replies by
    // itself: the callback owner must call SendPong() with the same payload.
    // Without a callback, the Pong is sent from the receive context.
    //
    // Callbacks run on the transport's receive context. For AT modems
    // (ML307/EC801E) that is the AT event task, which must parse the modem's
    // reply to any send, so a send issued synchronously from a callback stalls
    // until the AT command times out. Register OnPing on such transports and
    // send the Pong from another task.
    void OnPing(std::function<void(const char*, size_t)> callback);

private:
    NetworkResult<> Fail(NetworkError err) {
        last_error_ = err;
        return std::unexpected(err);
    }

    NetworkError last_error_{};
    NetworkInterface* network_;
    int connect_id_;
    std::unique_ptr<Tcp> tcp_;
    bool continuation_ = false;
    size_t receive_buffer_size_ = 2048;
    std::string receive_buffer_;
    bool handshake_completed_ = false;
    // Set when the peer violated the protocol; later data is dropped until
    // the next Connect().
    std::atomic<bool> receive_failed_ = false;
    std::atomic<bool> connected_ = false;

    // Mutex for sending data and replying pong
    std::mutex send_mutex_;
    
    EventGroupHandle_t handshake_event_group_;
    static const EventBits_t HANDSHAKE_SUCCESS_BIT = BIT0;
    static const EventBits_t HANDSHAKE_FAILED_BIT = BIT1;

    std::map<std::string, std::string> headers_;
    std::function<void(const char*, size_t, bool binary)> on_data_;
    std::function<void(const NetworkError& error)> on_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;
    std::function<void(const char*, size_t)> on_pong_;
    std::function<void(const char*, size_t)> on_ping_;

    std::vector<char> current_message_;
    bool is_fragmented_ = false;
    bool is_binary_ = false;

    void OnTcpData(const std::string& data);
    void FailReceive();
    bool SendControlFrame(uint8_t opcode, const void* data, size_t len);
};

#endif // WEBSOCKET_H
