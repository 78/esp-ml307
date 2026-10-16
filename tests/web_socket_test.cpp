// Exercises production WebSocket against a fake transport that answers the
// upgrade request synchronously. Run under ASan so teardown races surface as
// use-after-free reports.
#include "web_socket.h"
#include "network_interface.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

const char* NetworkError::Name() const { return "NetworkError"; }
const char* NetworkError::Message() const { return ""; }
std::string NetworkError::ToString() const { return std::to_string(static_cast<int>(code)); }

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #condition);                                           \
            std::abort();                                                       \
        }                                                                       \
    } while (0)

static const char* const kUpgradeResponse =
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";

class FakeTcp : public Tcp {
public:
    ~FakeTcp() override {
        // Models a receive task that keeps delivering until the transport
        // destructor joins it; real transports then report the disconnect.
        if (!late_data.empty() && stream_callback_) {
            stream_callback_(late_data);
        }
        Disconnect();
    }

    NetworkResult<> Connect(const std::string&, int) override {
        connected_ = true;
        return {};
    }
    void Disconnect() override {
        if (connected_.exchange(false) && disconnect_callback_) {
            disconnect_callback_();
        }
    }
    int Send(const std::string& data) override {
        sent += data;
        if (data.rfind("GET ", 0) == 0) {
            Deliver(handshake_response);
        }
        return static_cast<int>(data.size());
    }

    void Deliver(const std::string& data) { stream_callback_(data); }

    std::string handshake_response = kUpgradeResponse;
    std::string late_data;
    std::string sent;
};

class FakeNetwork : public NetworkInterface {
public:
    std::unique_ptr<Http> CreateHttp(int) override { return nullptr; }
    std::unique_ptr<Tcp> CreateTcp(int) override { return Create(); }
    std::unique_ptr<Tcp> CreateSsl(int) override { return Create(); }
    std::unique_ptr<Udp> CreateUdp(int) override { return nullptr; }
    std::unique_ptr<Mqtt> CreateMqtt(int) override { return nullptr; }
    std::unique_ptr<WebSocket> CreateWebSocket(int) override { return nullptr; }

    std::function<void(FakeTcp&)> configure;
    FakeTcp* last = nullptr;
    int created = 0;

private:
    std::unique_ptr<Tcp> Create() {
        auto tcp = std::make_unique<FakeTcp>();
        if (configure) {
            configure(*tcp);
        }
        last = tcp.get();
        ++created;
        return tcp;
    }
};

// Unmasked server frame with the shortest length encoding
static std::string Frame(uint8_t first_byte, const std::string& payload) {
    std::string frame(1, static_cast<char>(first_byte));
    size_t len = payload.size();
    if (len < 126) {
        frame.push_back(static_cast<char>(len));
    } else if (len <= 0xFFFF) {
        frame.push_back(static_cast<char>(126));
        frame.push_back(static_cast<char>(len >> 8));
        frame.push_back(static_cast<char>(len & 0xFF));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xFF));
        }
    }
    return frame + payload;
}

// Decodes one masked client frame; returns false if it is not a single
// unfragmented frame with a 7-bit length.
static bool DecodeClientFrame(const std::string& frame, uint8_t* opcode, std::string* payload) {
    if (frame.size() < 6 || (frame[0] & 0x80) == 0 || (frame[1] & 0x80) == 0) {
        return false;
    }
    size_t len = frame[1] & 0x7F;
    if (len > 125 || frame.size() != 6 + len) {
        return false;
    }
    *opcode = frame[0] & 0x0F;
    payload->clear();
    for (size_t i = 0; i < len; ++i) {
        payload->push_back(static_cast<char>(frame[6 + i] ^ frame[2 + i % 4]));
    }
    return true;
}

static void TestPingIsAnsweredWithoutHandler() {
    FakeNetwork network;
    WebSocket ws(&network, 1);
    CHECK(ws.Connect("ws://example.com/").has_value());

    size_t before = network.last->sent.size();
    network.last->Deliver(Frame(0x89, "hb"));
    uint8_t opcode = 0;
    std::string payload;
    CHECK(DecodeClientFrame(network.last->sent.substr(before), &opcode, &payload));
    CHECK(opcode == 0xA && payload == "hb");
}

static void TestPingHandlerOwnsPong() {
    FakeNetwork network;
    WebSocket ws(&network, 1);
    std::string pending;
    int pings = 0;
    ws.OnPing([&](const char* data, size_t len) {
        pending.assign(data, len);
        ++pings;
    });
    CHECK(ws.Connect("ws://example.com/").has_value());

    size_t before = network.last->sent.size();
    network.last->Deliver(Frame(0x89, "hb"));
    CHECK(pings == 1 && pending == "hb");
    CHECK(network.last->sent.size() == before);

    CHECK(ws.SendPong(pending.data(), pending.size()));
    uint8_t opcode = 0;
    std::string payload;
    CHECK(DecodeClientFrame(network.last->sent.substr(before), &opcode, &payload));
    CHECK(opcode == 0xA && payload == "hb");

    CHECK(!ws.SendPong(std::string(126, 'p').data(), 126));
}

static void TestReconnectOnSameObject() {
    FakeNetwork network;
    WebSocket ws(&network, 1);
    std::string received;
    ws.OnData([&](const char* data, size_t len, bool) { received.assign(data, len); });

    CHECK(ws.Connect("ws://example.com/ws").has_value());
    network.last->Deliver(Frame(0x81, "one"));
    CHECK(received == "one");

    // Leftover bytes from the first connection must not leak into the second
    network.last->Deliver(std::string("\x81\x05par", 5));

    const auto started = std::chrono::steady_clock::now();
    CHECK(ws.Connect("ws://example.com/ws").has_value());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    CHECK(network.created == 2);
    CHECK(ws.IsConnected());

    network.last->Deliver(Frame(0x81, "two"));
    CHECK(received == "two");
}

static void TestFramesAfterUpgradeInSamePacket() {
    FakeNetwork network;
    network.configure = [](FakeTcp& tcp) {
        tcp.handshake_response = std::string(kUpgradeResponse) + Frame(0x82, "bin");
    };
    WebSocket ws(&network, 1);
    std::string received;
    bool binary = false;
    ws.OnData([&](const char* data, size_t len, bool is_binary) {
        received.assign(data, len);
        binary = is_binary;
    });
    CHECK(ws.Connect("wss://example.com/").has_value());
    CHECK(received == "bin" && binary);
}

static void TestHugeLengthFailsConnection() {
    FakeNetwork network;
    WebSocket ws(&network, 1);
    int errors = 0;
    int disconnects = 0;
    ws.OnError([&](const NetworkError& error) {
        CHECK(error.code == NetworkErrc::ProtocolError);
        ++errors;
    });
    ws.OnDisconnected([&] { ++disconnects; });
    CHECK(ws.Connect("ws://example.com/").has_value());

    // 64-bit length close to 2^64: header_length + payload_length would wrap
    std::string header("\x82\x7F", 2);
    header.append(8, static_cast<char>(0xFF));
    network.last->Deliver(header);
    CHECK(!ws.IsConnected());
    CHECK(errors == 1 && disconnects == 1);

    // Later data is dropped instead of being buffered without bound
    network.last->Deliver(std::string(4096, 'x'));
    network.last->Disconnect();
    CHECK(disconnects == 1);
}

static void TestFragmentedMessageIsBounded() {
    FakeNetwork network;
    WebSocket ws(&network, 1);
    int errors = 0;
    bool delivered = false;
    ws.OnError([&](const NetworkError&) { ++errors; });
    ws.OnData([&](const char*, size_t, bool) { delivered = true; });
    CHECK(ws.Connect("ws://example.com/").has_value());

    const std::string chunk(60 * 1024, 'a');
    network.last->Deliver(Frame(0x02, chunk));
    for (size_t sent = chunk.size(); sent <= WebSocket::MAX_MESSAGE_SIZE && errors == 0; sent += chunk.size()) {
        network.last->Deliver(Frame(0x00, chunk));
    }
    CHECK(errors == 1);
    CHECK(!delivered);
    CHECK(!ws.IsConnected());
}

static void TestMessageAtLimitIsDelivered() {
    FakeNetwork network;
    WebSocket ws(&network, 1);
    size_t received = 0;
    ws.OnData([&](const char*, size_t len, bool) { received = len; });
    CHECK(ws.Connect("ws://example.com/").has_value());
    network.last->Deliver(Frame(0x82, std::string(WebSocket::MAX_MESSAGE_SIZE, 'b')));
    CHECK(received == WebSocket::MAX_MESSAGE_SIZE);
    CHECK(ws.IsConnected());
}

static void TestFailedHandshakeClosesTransport() {
    FakeNetwork network;
    network.configure = [](FakeTcp& tcp) {
        tcp.handshake_response = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
    };
    WebSocket ws(&network, 1);
    auto result = ws.Connect("ws://example.com/");
    CHECK(!result.has_value());
    CHECK(result.error().code == NetworkErrc::ProtocolError);
    CHECK(!network.last->connected());
}

static void TestDestroyJoinsTransportFirst() {
    // Data that arrives while the transport is being torn down must find the
    // WebSocket members (buffers, callbacks, event group) still alive.
    FakeNetwork network;
    network.configure = [](FakeTcp& tcp) { tcp.late_data = Frame(0x81, "late"); };
    std::string received;
    int disconnects = 0;
    {
        WebSocket ws(&network, 1);
        ws.OnData([&](const char* data, size_t len, bool) { received.assign(data, len); });
        ws.OnDisconnected([&] { ++disconnects; });
        CHECK(ws.Connect("ws://example.com/").has_value());
    }
    CHECK(disconnects == 1);
    CHECK(received == "late");
}

static void TestDestroyAfterFailedHandshake() {
    // connected_ stays false here, so the transport must still be torn down
    // before the event group is deleted (the late 101 would set its bits).
    FakeNetwork network;
    network.configure = [](FakeTcp& tcp) {
        tcp.handshake_response = "HTTP/1.1 403 Forbidden\r\n\r\n";
        tcp.late_data = kUpgradeResponse;
    };
    auto ws = std::make_unique<WebSocket>(&network, 1);
    CHECK(!ws->Connect("ws://example.com/").has_value());
    ws.reset();
}

int main() {
    TestReconnectOnSameObject();
    TestFramesAfterUpgradeInSamePacket();
    TestHugeLengthFailsConnection();
    TestFragmentedMessageIsBounded();
    TestMessageAtLimitIsDelivered();
    TestFailedHandshakeClosesTransport();
    TestDestroyJoinsTransportFirst();
    TestDestroyAfterFailedHandshake();
    TestPingIsAnsweredWithoutHandler();
    TestPingHandlerOwnsPong();
    std::puts("web_socket tests passed");
    return 0;
}
