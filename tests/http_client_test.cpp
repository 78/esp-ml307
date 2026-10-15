// Exercises production HttpClient against a fake transport. Header stubs live
// in tests/stubs; the event group blocks for real so missed wake-ups surface as
// timeouts instead of passing silently.
#include "http_client.h"
#include "network_interface.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

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

class FakeTcp : public Tcp {
public:
    NetworkResult<> Connect(const std::string&, int) override {
        connected_ = true;
        return {};
    }
    void Disconnect() override { connected_ = false; }
    int Send(const std::string& data) override {
        sent += data;
        return static_cast<int>(data.size());
    }

    void Deliver(const std::string& data) { stream_callback_(data); }

    // Mirrors EspSsl::ReceiveTask after a TLS read error or peer close.
    void PeerClose() {
        connected_ = false;
        disconnect_callback_();
    }

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

    FakeTcp* last = nullptr;
    int created = 0;

private:
    std::unique_ptr<Tcp> Create() {
        auto tcp = std::make_unique<FakeTcp>();
        last = tcp.get();
        ++created;
        return tcp;
    }
};

static bool FinishesWithin(std::chrono::milliseconds limit, const std::function<void()>& work) {
    const auto started = std::chrono::steady_clock::now();
    work();
    return std::chrono::steady_clock::now() - started < limit;
}

static std::string ReadOnce(HttpClient& client) {
    char buffer[64];
    auto read = client.Read(buffer, sizeof(buffer));
    CHECK(read.has_value());
    return std::string(buffer, static_cast<size_t>(*read));
}

static void TestCloseBeforeHeadersFailsImmediately() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(10000);
    CHECK(client.Open("GET", "https://example.com/image.png").has_value());
    CHECK(network.last->sent.rfind("GET /image.png HTTP/1.1\r\n", 0) == 0);

    const auto started = std::chrono::steady_clock::now();
    std::thread receive_task([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        network.last->PeerClose();
    });
    auto status = client.GetStatusCode();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    receive_task.join();

    CHECK(!status.has_value());
    CHECK(status.error().code == NetworkErrc::ServerDisconnected);
    CHECK(elapsed < std::chrono::seconds(2));

    char buffer[16];
    auto read = client.Read(buffer, sizeof(buffer));
    CHECK(!read.has_value());
    CHECK(read.error().code == NetworkErrc::ServerDisconnected);
}

static void TestCompleteResponseThenCloseSucceeds() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(1000);
    CHECK(client.Open("GET", "https://example.com/ok").has_value());

    network.last->Deliver("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    network.last->PeerClose();

    auto status = client.GetStatusCode();
    CHECK(status.has_value() && *status == 200);
    CHECK(client.ReadAll() == "ok");
}

static void TestTruncatedBodyReportsDisconnect() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(1000);
    CHECK(client.Open("GET", "https://example.com/partial").has_value());

    network.last->Deliver("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
    network.last->PeerClose();

    auto status = client.GetStatusCode();
    CHECK(status.has_value() && *status == 200);
    char buffer[16];
    auto read = client.Read(buffer, sizeof(buffer));
    CHECK(!read.has_value());
    CHECK(read.error().code == NetworkErrc::ServerDisconnected);
}

static void TestReopenAfterEarlyCloseClearsError() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(1000);
    CHECK(client.Open("GET", "https://example.com/first").has_value());
    network.last->PeerClose();
    CHECK(!client.GetStatusCode().has_value());

    CHECK(client.Open("GET", "https://example.com/second").has_value());
    network.last->Deliver("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
    auto status = client.GetStatusCode();
    CHECK(status.has_value() && *status == 204);
}

// The connection stays open in these cases, so Read() must not wait for a
// close or for the timeout to learn that the body is empty.
static void TestBodylessResponsesCompleteAtHeaders() {
    struct Case {
        const char* method;
        const char* response;
    };
    const Case cases[] = {
        {"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n"},
        {"GET", "HTTP/1.1 204 No Content\r\n\r\n"},
        {"GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 10\r\n\r\n"},
        {"GET", "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"},
    };
    for (const auto& test_case : cases) {
        FakeNetwork network;
        HttpClient client(&network);
        client.SetTimeout(5000);
        CHECK(client.Open(test_case.method, "https://example.com/empty").has_value());
        network.last->Deliver(test_case.response);
        CHECK(FinishesWithin(std::chrono::seconds(1), [&] { CHECK(ReadOnce(client).empty()); }));
    }
}

static void TestChunkedResponseAcrossPackets() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(5000);
    CHECK(client.Open("GET", "https://example.com/chunked").has_value());

    network.last->Deliver("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n5\r\nhel");
    network.last->Deliver("lo\r");
    network.last->Deliver("\n6;ext=1\r\n world\r\n0\r\n");
    network.last->Deliver("\r\n");

    CHECK(client.GetBodyLength() == 0);
    CHECK(FinishesWithin(std::chrono::seconds(1), [&] { CHECK(client.ReadAll() == "hello world"); }));
}

static void TestInvalidChunkSizeFails() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(5000);
    CHECK(client.Open("GET", "https://example.com/bad-chunk").has_value());
    network.last->Deliver("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n");

    CHECK(FinishesWithin(std::chrono::seconds(1), [&] {
        char buffer[16];
        auto read = client.Read(buffer, sizeof(buffer));
        CHECK(!read.has_value());
        CHECK(read.error().code == NetworkErrc::ProtocolError);
    }));
}

static void TestInvalidResponseHeadFails() {
    const char* responses[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 12abc\r\n\r\n",
        "SSH-2.0-OpenSSH\r\n",
    };
    for (const char* response : responses) {
        FakeNetwork network;
        HttpClient client(&network);
        client.SetTimeout(5000);
        CHECK(client.Open("GET", "https://example.com/bad").has_value());
        network.last->Deliver(response);
        auto status = client.GetStatusCode();
        CHECK(!status.has_value());
        CHECK(status.error().code == NetworkErrc::ProtocolError);
    }
}

static void TestOverlongLineFails() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(5000);
    CHECK(client.Open("GET", "https://example.com/long").has_value());
    network.last->Deliver("HTTP/1.1 200 OK\r\nX-Long: " + std::string(9000, 'a'));

    auto status = client.GetStatusCode();
    CHECK(!status.has_value());
    CHECK(status.error().code == NetworkErrc::ProtocolError);
}

static void TestBodyIsTrimmedToContentLength() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(1000);
    CHECK(client.Open("GET", "https://example.com/trim").has_value());
    network.last->Deliver("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabcEXTRA");
    CHECK(client.ReadAll() == "abc");
}

static void TestInterimResponseIsSkipped() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(1000);
    CHECK(client.Open("GET", "https://example.com/hints").has_value());
    network.last->Deliver("HTTP/1.1 103 Early Hints\r\nLink: </a.css>\r\n\r\n"
                          "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    auto status = client.GetStatusCode();
    CHECK(status.has_value() && *status == 200);
    CHECK(client.GetResponseHeader("Link").empty());
    CHECK(client.ReadAll() == "ok");
}

static void TestCloseDelimitedBody() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(1000);
    CHECK(client.Open("GET", "https://example.com/legacy").has_value());
    network.last->Deliver("HTTP/1.1 200 OK\r\n\r\nuntil ");
    network.last->Deliver("close");
    network.last->PeerClose();
    CHECK(client.ReadAll() == "until close");
}

static void TestEmptyReadBufferDoesNotConsume() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(1000);
    CHECK(client.Open("GET", "https://example.com/zero").has_value());
    network.last->Deliver("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");

    char unused = 0;
    auto read = client.Read(&unused, 0);
    CHECK(read.has_value() && *read == 0);
    CHECK(client.ReadAll() == "ok");
}

static void TestKeepAliveReuseRequiresSameEndpoint() {
    FakeNetwork network;
    HttpClient client(&network);
    client.SetTimeout(1000);
    client.SetKeepAlive(true);
    auto request = [&](const char* url) {
        CHECK(client.Open("GET", url).has_value());
        network.last->Deliver("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 2\r\n\r\nok");
        CHECK(client.ReadAll() == "ok");
    };

    request("https://a.example.com/1");
    CHECK(network.created == 1);
    request("https://b.example.com/1");
    CHECK(network.created == 2);
    request("https://b.example.com/2");
    CHECK(network.created == 2);
    request("http://b.example.com/3");
    CHECK(network.created == 3);
}

int main() {
    TestCloseBeforeHeadersFailsImmediately();
    TestCompleteResponseThenCloseSucceeds();
    TestTruncatedBodyReportsDisconnect();
    TestReopenAfterEarlyCloseClearsError();
    TestBodylessResponsesCompleteAtHeaders();
    TestChunkedResponseAcrossPackets();
    TestInvalidChunkSizeFails();
    TestInvalidResponseHeadFails();
    TestOverlongLineFails();
    TestBodyIsTrimmedToContentLength();
    TestInterimResponseIsSkipped();
    TestCloseDelimitedBody();
    TestEmptyReadBufferDoesNotConsume();
    TestKeepAliveReuseRequiresSameEndpoint();
    std::puts("http_client tests passed");
    return 0;
}
