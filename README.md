# ML307 / Quectel-E Series Cat.1 AT Modem (v3.7)

English | [简体中文](README_zh.md)

ESP-IDF component for ML307R / EC801E / NT26K LTE Cat.1 modules.
Originally created for https://github.com/78/xiaozhi-esp32.

If you see `UART_FIFO_OVF`, set `CONFIG_UART_ISR_IN_IRAM=y` and keep heavy I/O such as LVGL on CPU1.

Requires **C++23** (`std::expected`) and **ESP-IDF >= 5.5.2**.

Version 3.7.5 requires **uart-uhci ^0.4.0** on non-ESP32 targets and can share it with uart-eth-modem 0.7.x. Public AT APIs are unchanged. UART transmission has a separate timeout based on 8N1 wire time plus 1000 ms; the command timeout still controls waiting for the modem response. See [CHANGELOG.md](CHANGELOG.md) for release notes.

## What's New in 3.7

- **Structured errors**: sync APIs return `NetworkResult<>` / `AtResult` (`std::expected`). `bool` + `GetLastError()` is gone.
- **Actionable failure reasons**: DNS, timeout, TLS, auth, CME, and similar failures map to `NetworkErrc`. `ToString()` is safe to show to users.
- **Async errors use callbacks**: WebSocket `OnError` receives `const NetworkError&`; MQTT `OnError` still receives a readable string.

```cpp
auto opened = http->Open("GET", "https://example.com/ota.json");
if (!opened) {
    // e.g. "DNS resolution failed (dns_failed, native=1)"
    ESP_LOGE(TAG, "OTA request failed: %s", opened.error().ToString().c_str());
    return;
}
```

## What's New in 3.5

- Low-power mode for the cellular module
- DTR pin wakes the 4G module from the MCU
- RI pin wakes the MCU from the 4G module
- EC801E idle current on-network is about 1–2 mA

Enable these options for low-power mode:

- `CONFIG_PM_ENABLE=y`
- `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`

## What's New in 3.0

- Automatic module detection for ML307 and EC801E
- Shared `NetworkInterface` API
- `std::unique_ptr` ownership
- Simpler client creation

## Features

- AT commands
- MQTT / MQTTS
- HTTP / HTTPS
- TCP / SSL TCP
- UDP
- WebSocket
- Automatic module detect and init
- Structured network / AT errors

## Supported Modules

- ML307R
- ML307A
- EC801E \*
- NT26K \*

\* Confirm with the vendor that the firmware includes SSL TCP support.

## Quick Start

### Basic Usage

```cpp
#include "esp_log.h"
#include "at_modem.h"

static const char *TAG = "ML307_DEMO";

extern "C" void app_main(void) {
    auto detected = AtModem::Detect(GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15, 921600);
    if (!detected) {
        ESP_LOGE(TAG, "Modem detect failed: %s", detected.error().ToString().c_str());
        return;
    }
    auto modem = std::move(*detected);

    modem->OnNetworkStateChanged([](bool ready) {
        ESP_LOGI(TAG, "Network: %s", ready ? "ready" : "down");
    });

    NetworkStatus status = modem->WaitForNetworkReady(30000);
    if (status != NetworkStatus::Ready) {
        ESP_LOGE(TAG, "Network not ready");
        return;
    }

    ESP_LOGI(TAG, "Revision: %s", modem->GetModuleRevision().c_str());
    ESP_LOGI(TAG, "IMEI: %s", modem->GetImei().c_str());
    ESP_LOGI(TAG, "ICCID: %s", modem->GetIccid().c_str());
    ESP_LOGI(TAG, "Carrier: %s", modem->GetCarrierName().c_str());
    ESP_LOGI(TAG, "CSQ: %d", modem->GetCsq());
}
```

### HTTP Client

```cpp
void TestHttp(std::unique_ptr<AtModem>& modem) {
    auto http = modem->CreateHttp(0);
    http->SetHeader("User-Agent", "Xiaozhi/3.7.0");
    http->SetTimeout(10000);

    auto opened = http->Open("GET", "https://httpbin.org/json");
    if (!opened) {
        ESP_LOGE(TAG, "HTTP open failed: %s", opened.error().ToString().c_str());
        return;
    }

    auto status = http->GetStatusCode();
    if (!status) {
        ESP_LOGE(TAG, "HTTP status failed: %s", status.error().ToString().c_str());
        return;
    }
    ESP_LOGI(TAG, "HTTP status: %d, body: %zu bytes", *status, http->GetBodyLength());

    std::string response = http->ReadAll();
    ESP_LOGI(TAG, "Body: %s", response.c_str());
    http->Close();
}
```

`Read()` returns the number of bytes on success (`0` means EOF) and a `NetworkError` on failure:

```cpp
char buffer[512];
while (true) {
    auto n = http->Read(buffer, sizeof(buffer));
    if (!n) {
        ESP_LOGE(TAG, "HTTP read failed: %s", n.error().ToString().c_str());
        break;
    }
    if (*n == 0) {
        break;
    }
    // consume buffer[0 .. *n)
}
```

### MQTT Client

```cpp
void TestMqtt(std::unique_ptr<AtModem>& modem) {
    auto mqtt = modem->CreateMqtt(0);

    mqtt->OnConnected([]() {
        ESP_LOGI(TAG, "MQTT connected");
    });
    mqtt->OnDisconnected([]() {
        ESP_LOGI(TAG, "MQTT disconnected");
    });
    mqtt->OnMessage([](const std::string& topic, const std::string& payload) {
        ESP_LOGI(TAG, "MQTT [%s]: %s", topic.c_str(), payload.c_str());
    });
    mqtt->OnError([](const std::string& error) {
        ESP_LOGE(TAG, "MQTT error: %s", error.c_str());
    });

    auto connected = mqtt->Connect("broker.emqx.io", 1883, "esp32_client", "", "");
    if (!connected) {
        ESP_LOGE(TAG, "MQTT connect failed: %s", connected.error().ToString().c_str());
        return;
    }

    mqtt->Subscribe("test/esp32/message");
    mqtt->Publish("test/esp32/hello", "Hello from ESP32!");
    vTaskDelay(pdMS_TO_TICKS(5000));
    mqtt->Disconnect();
}
```

### WebSocket Client

```cpp
void TestWebSocket(std::unique_ptr<AtModem>& modem) {
    auto ws = modem->CreateWebSocket(0);
    ws->SetHeader("Protocol-Version", "3");

    ws->OnConnected([]() {
        ESP_LOGI(TAG, "WebSocket connected");
    });
    ws->OnData([](const char* data, size_t length, bool binary) {
        ESP_LOGI(TAG, "WebSocket data: %.*s", (int)length, data);
    });
    ws->OnDisconnected([]() {
        ESP_LOGI(TAG, "WebSocket disconnected");
    });
    ws->OnError([](const NetworkError& error) {
        ESP_LOGE(TAG, "WebSocket error: %s", error.ToString().c_str());
    });

    auto connected = ws->Connect("wss://echo.websocket.org/");
    if (!connected) {
        ESP_LOGE(TAG, "WebSocket connect failed: %s", connected.error().ToString().c_str());
        return;
    }

    for (int i = 0; i < 5; i++) {
        std::string message = "{\"type\": \"ping\", \"id\": " + std::to_string(i) + "}";
        ws->Send(message);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ws->Close();
}
```

### TCP Client

```cpp
void TestTcp(std::unique_ptr<AtModem>& modem) {
    auto tcp = modem->CreateTcp(0);
    tcp->OnStream([](const std::string& data) {
        ESP_LOGI(TAG, "TCP recv: %s", data.c_str());
    });
    tcp->OnDisconnected([]() {
        ESP_LOGI(TAG, "TCP disconnected");
    });

    auto connected = tcp->Connect("httpbin.org", 80);
    if (!connected) {
        ESP_LOGE(TAG, "TCP connect failed: %s", connected.error().ToString().c_str());
        return;
    }

    std::string request = "GET /ip HTTP/1.1\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n";
    int sent = tcp->Send(request);
    ESP_LOGI(TAG, "TCP sent %d bytes", sent);
    vTaskDelay(pdMS_TO_TICKS(3000));
    tcp->Disconnect();
}
```

### UDP Client

```cpp
void TestUdp(std::unique_ptr<AtModem>& modem) {
    auto udp = modem->CreateUdp(0);
    udp->OnMessage([](const std::string& data) {
        ESP_LOGI(TAG, "UDP recv: %s", data.c_str());
    });

    auto connected = udp->Connect("8.8.8.8", 53);
    if (!connected) {
        ESP_LOGE(TAG, "UDP connect failed: %s", connected.error().ToString().c_str());
        return;
    }

    int sent = udp->Send("Hello UDP Server!");
    ESP_LOGI(TAG, "UDP sent %d bytes", sent);
    vTaskDelay(pdMS_TO_TICKS(2000));
    udp->Disconnect();
}
```

## Error Handling

Synchronous results are in the return value. Do not call `GetLastError()`; that API was removed.

| Type | Definition | Used by |
|---|---|---|
| `NetworkResult<T>` | `std::expected<T, NetworkError>` | HTTP / TCP / UDP / MQTT / WebSocket |
| `NetworkResult<>` | `std::expected<void, NetworkError>` | `Open()` / `Connect()` |
| `AtResult` | `std::expected<void, AtError>` | `AtUart::SendCommand()` / `SetBaudRate()` |
| `AtValue<T>` | `std::expected<T, AtError>` | `AtModem::Detect()` |

`NetworkError` keeps both a category and the native code:

```cpp
enum class NetworkErrc {
    InvalidArgument,
    NotInitialized,
    Timeout,
    DnsFailed,
    ConnectFailed,
    TlsFailed,
    ProtocolError,
    AuthRejected,
    ServerDisconnected,
    NetworkUnavailable,
    TransmitFailed,
    ReceiveFailed,
    AtCommandFailed,
    CmeError,
    HttpFailed,
    Unknown,
};

struct NetworkError {
    NetworkErrc code;
    int native;          // errno / esp_err_t / module-specific code
    const char* Name() const;     // "dns_failed"
    const char* Message() const;  // "DNS resolution failed"
    std::string ToString() const; // "DNS resolution failed (dns_failed, native=1)"
};
```

Keep `native` for vendor manuals. Prefer `ToString()` or `Message()` in logs and UI.

Branch on the category when you need different retry behavior:

```cpp
auto opened = http->Open("GET", url);
if (!opened) {
    const auto& err = opened.error();
    switch (err.code) {
        case NetworkErrc::DnsFailed:
            ESP_LOGE(TAG, "DNS failed: %s", err.ToString().c_str());
            break;
        case NetworkErrc::Timeout:
            ESP_LOGE(TAG, "Timed out: %s", err.ToString().c_str());
            break;
        case NetworkErrc::TlsFailed:
            ESP_LOGE(TAG, "TLS failed: %s", err.ToString().c_str());
            break;
        default:
            ESP_LOGE(TAG, "Request failed: %s", err.ToString().c_str());
            break;
    }
}
```

AT errors can be converted to network errors:

```cpp
auto uart = modem->GetAtUart();
if (auto result = uart->SendCommand("AT+CSQ", 1000); result) {
    ESP_LOGI(TAG, "CSQ: %s", uart->GetResponse().c_str());
} else {
    NetworkError net = result.error().ToNetworkError();
    ESP_LOGE(TAG, "AT failed: %s / %s",
             result.error().ToString().c_str(), net.ToString().c_str());
}
```

After a connection is established, later failures go through callbacks:

- WebSocket: `OnError(const NetworkError&)`
- MQTT: `OnError(const std::string&)`
- TCP: `OnDisconnected()`
- Modem: `OnNetworkStateChanged(bool)`

Network attach still uses `NetworkStatus` (SIM / registration / timeout). That is separate from transport-level `NetworkError`:

```cpp
switch (modem->WaitForNetworkReady(30000)) {
    case NetworkStatus::Ready:
        break;
    case NetworkStatus::ErrorInsertPin:
        ESP_LOGE(TAG, "SIM missing or PIN error");
        break;
    case NetworkStatus::ErrorRegistrationDenied:
        ESP_LOGE(TAG, "Registration denied");
        break;
    case NetworkStatus::ErrorTimeout:
        ESP_LOGE(TAG, "Network attach timed out");
        break;
    default:
        ESP_LOGE(TAG, "Unknown network attach error");
        break;
}
```

## Advanced Usage

### Direct AtUart Access

```cpp
void DirectAtCommand(std::unique_ptr<AtModem>& modem) {
    auto uart = modem->GetAtUart();
    if (auto result = uart->SendCommand("AT+CSQ", 1000); result) {
        ESP_LOGI(TAG, "CSQ: %s", uart->GetResponse().c_str());
    } else {
        ESP_LOGE(TAG, "AT+CSQ failed: %s", result.error().ToString().c_str());
    }

    std::shared_ptr<AtUart> my_uart = modem->GetAtUart();
}
```

### Network State Monitoring

```cpp
void MonitorNetwork(std::unique_ptr<AtModem>& modem) {
    modem->OnNetworkStateChanged([&modem](bool ready) {
        if (ready) {
            ESP_LOGI(TAG, "Network ready, CSQ=%d, CEREG=%s",
                     modem->GetCsq(), modem->GetRegistrationState().ToString().c_str());
        } else {
            ESP_LOGE(TAG, "Network lost");
        }
    });
}
```

### Releasing Clients Early

```cpp
void EarlyReleaseExample(std::unique_ptr<AtModem>& modem) {
    auto http = modem->CreateHttp(0);
    http->Close();
    http.reset();

    {
        auto tcp = modem->CreateTcp(0);
        (void)tcp->Connect("example.com", 80);
    }

    auto udp = modem->CreateUdp(0);
}
```

## Migration (v3.6 → v3.7)

3.7 is not compatible with `bool` / `GetLastError()`. Callers must switch to `std::expected`.

```cpp
// v3.6
if (!http->Open("GET", url)) {
    ESP_LOGE(TAG, "failed, code=%d", http->GetLastError());
}

// v3.7
if (auto opened = http->Open("GET", url); !opened) {
    ESP_LOGE(TAG, "failed: %s", opened.error().ToString().c_str());
}
```

Signature changes:

| API | v3.6 | v3.7 |
|---|---|---|
| `AtModem::Detect` | `std::unique_ptr<AtModem>` / `nullptr` | `AtValue<std::unique_ptr<AtModem>>` |
| `Http::Open` / `Tcp::Connect` / `Udp::Connect` / `Mqtt::Connect` / `WebSocket::Connect` | `bool` | `NetworkResult<>` |
| `Http::Read` / `Write` / `GetStatusCode` | `int` (negative on failure) | `NetworkResult<int>` |
| `AtUart::SendCommand` / `SetBaudRate` | `bool` | `AtResult` |
| `GetLastError()` / `GetCmeErrorCode()` | public API | **removed** |
| `WebSocket::OnError` | `void(int)` | `void(const NetworkError&)` |

`std::expected` converts to `bool` only explicitly. If a wrapper still returns `bool`, use `.has_value()`:

```cpp
return at_uart_->SendCommand("AT+QSCLK=1").has_value();
```

Dependency:

```yaml
dependencies:
  78/esp-ml307: "~3.7.0"
```

## Migration (v2.x → v3.0)

```cpp
// v2.x
Ml307AtModem modem(GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15);
NetworkStatus status = modem.WaitForNetworkReady();
Ml307Http http(modem);
http.Open("GET", "https://example.com");

// v3.7
auto detected = AtModem::Detect(GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15);
if (!detected) {
    return;
}
auto modem = std::move(*detected);
NetworkStatus status = modem->WaitForNetworkReady();
auto http = modem->CreateHttp(0);
if (auto opened = http->Open("GET", "https://example.com"); !opened) {
    ESP_LOGE(TAG, "%s", opened.error().ToString().c_str());
}
```

## Design Notes

1. Module type is detected automatically
2. All modules share the same client API
3. Sync failures carry a reason, so OTA no longer reports only `code=-1`
4. `std::unique_ptr` owns protocol clients
5. New module backends can be added behind `NetworkInterface`

## Notes

1. Create the modem with `AtModem::Detect()` and check the `AtValue`
2. Create protocol clients with `CreateXxx()`; they return `std::unique_ptr`
3. Sync failures are in the return value; later disconnects and protocol errors use callbacks
4. `GetAtUart()` returns `shared_ptr<AtUart>` and can be shared safely
5. Call `.reset()` to release a client early
6. Network interface methods default to `connect_id = -1`
7. Translation units need C++23; the component exports `PUBLIC cxx_std_23`

## Author

- Terrence (terrence@tenclass.com)
