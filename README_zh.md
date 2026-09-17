# ML307 / Quectel-E Series Cat.1 AT Modem (v3.7)

[English](README.md) | 简体中文

这是一个适用于 ML307R / EC801E / NT26K LTE Cat.1 模组的组件。
本项目最初为 https://github.com/78/xiaozhi-esp32 项目创建。

出现 UART_FIFO_OVF 需要设置 `CONFIG_UART_ISR_IN_IRAM=y`，其他 IO 如 LVGL 放在 CPU1。

组件要求 **C++23**（`std::expected`）和 **ESP-IDF >= 5.5.2**。

3.7.5 在非 ESP32 目标上要求 **uart-uhci ^0.4.0**，可以与 uart-eth-modem 0.7.x 共用此依赖。公开 AT API 不变。UART 发送超时按 8N1 传输时间加 1000 ms 单独计算；命令超时参数仍控制等待模组响应的时间。版本记录见 [CHANGELOG.md](CHANGELOG.md)。

## 🆕 版本 3.7 新特性

- **结构化错误**: 同步接口返回 `NetworkResult<>` / `AtResult`（`std::expected`），不再用 `bool` + `GetLastError()`
- **可区分失败原因**: DNS、超时、TLS、认证拒绝、CME 等映射到 `NetworkErrc`，`ToString()` 可直接展示给用户
- **异步错误走回调**: WebSocket `OnError` 接收 `const NetworkError&`；MQTT `OnError` 仍接收可读字符串

```cpp
auto opened = http->Open("GET", "https://example.com/ota.json");
if (!opened) {
    // e.g. "DNS resolution failed (dns_failed, native=1)"
    ESP_LOGE(TAG, "OTA request failed: %s", opened.error().ToString().c_str());
    return;
}
```

## 🆕 版本 3.5 新特性

- **低功耗模式支持**: 支持模组进入低功耗模式，大幅降低待机功耗
- **DTR 唤醒功能**: DTR 引脚用于 MCU 唤醒 4G 模组
- **RI 唤醒功能**: RI 引脚用于 4G 模组唤醒 MCU
- **超低待机功耗**: EC801E 实测驻网待机电流 1~2mA

> **注意**: 使用低功耗模式需要开启以下配置：
> - `CONFIG_PM_ENABLE=y`
> - `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`

## 🆕 版本 3.0 新特性

- **自动模组检测**: 自动识别 ML307 和 EC801E 模组
- **统一接口**: 通过 `NetworkInterface` 基类提供一致的 API
- **智能内存管理**: 使用 `std::unique_ptr` 确保内存安全
- **简化的 API**: 更加直观和易用的接口设计

## 功能特性

- AT 命令
- MQTT / MQTTS
- HTTP / HTTPS
- TCP / SSL TCP
- UDP
- WebSocket
- 自动模组检测和初始化
- 结构化网络 / AT 错误

## 支持的模组

- ML307R
- ML307A
- EC801E \*
- NT26K \*

\* 需要在购买时咨询是否已烧录支持 SSL TCP 的固件

## 快速开始

### 基础用法

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

### HTTP 客户端

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

分块读取时，`Read()` 成功返回已读字节数（`0` 表示结束），失败返回 `NetworkError`：

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

### MQTT 客户端

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

### WebSocket 客户端

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

### TCP 客户端

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

### UDP 客户端

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

## 错误处理

同步调用的结果在返回值里，不要再查询 `GetLastError()`（该接口已删除）。

| 类型 | 定义 | 用途 |
|---|---|---|
| `NetworkResult<T>` | `std::expected<T, NetworkError>` | HTTP / TCP / UDP / MQTT / WebSocket |
| `NetworkResult<>` | `std::expected<void, NetworkError>` | `Open()` / `Connect()` 等无返回值接口 |
| `AtResult` | `std::expected<void, AtError>` | `AtUart::SendCommand()` / `SetBaudRate()` |
| `AtValue<T>` | `std::expected<T, AtError>` | `AtModem::Detect()` |

`NetworkError` 包含分类码和底层原始码：

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

`native` 保留模组或系统原始错误，便于对照手册；展示给用户时优先用 `ToString()` 或 `Message()`。

按类别分支处理：

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

AT 层错误可以转成网络错误：

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

连接建立之后的失败走回调，而不是再次调用同步接口：

- WebSocket: `OnError(const NetworkError&)`
- MQTT: `OnError(const std::string&)`
- TCP: `OnDisconnected()`
- Modem: `OnNetworkStateChanged(bool)`

驻网阶段仍使用 `NetworkStatus`（SIM / 注册 / 超时），与传输层的 `NetworkError` 分开：

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

## 高级用法

### 直接访问 AtUart

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

### 网络状态监控

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

### 提前释放网络对象

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

## 迁移指南 (v3.6 → v3.7)

3.7 不兼容旧的 `bool` / `GetLastError()` 用法，调用方必须改成 `std::expected`。

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

主要签名变化：

| API | v3.6 | v3.7 |
|---|---|---|
| `AtModem::Detect` | `std::unique_ptr<AtModem>` / `nullptr` | `AtValue<std::unique_ptr<AtModem>>` |
| `Http::Open` / `Tcp::Connect` / `Udp::Connect` / `Mqtt::Connect` / `WebSocket::Connect` | `bool` | `NetworkResult<>` |
| `Http::Read` / `Write` / `GetStatusCode` | `int`（失败为负数） | `NetworkResult<int>` |
| `AtUart::SendCommand` / `SetBaudRate` | `bool` | `AtResult` |
| `GetLastError()` / `GetCmeErrorCode()` | 公开接口 | **已删除** |
| `WebSocket::OnError` | `void(int)` | `void(const NetworkError&)` |

`std::expected` 只允许显式转 `bool`。若外层函数仍返回 `bool`，写成 `.has_value()`：

```cpp
return at_uart_->SendCommand("AT+QSCLK=1").has_value();
```

依赖声明：

```yaml
dependencies:
  78/esp-ml307: "~3.7.0"
```

## 迁移指南 (v2.x → v3.0)

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

## 架构优势

1. **自动化**: 无需手动指定模组类型，提高代码通用性
2. **统一接口**: 不同模组使用相同的 API
3. **明确错误**: 同步失败原因随返回值一起给出，OTA 等场景不再只看到 `code=-1`
4. **内存安全**: `std::unique_ptr` 提供自动内存管理
5. **扩展性**: 便于添加新的模组类型支持

## 注意事项

1. 使用 `AtModem::Detect()` 创建模组实例，并检查 `AtValue`
2. 协议客户端通过 `CreateXxx()` 创建，返回 `std::unique_ptr`
3. 同步失败看返回值；连接后的断开 / 协议错误看回调
4. `GetAtUart()` 返回 `shared_ptr<AtUart>`，支持安全共享
5. 提前释放网络对象时调用 `.reset()`
6. 所有网络接口方法现在都有默认参数 `connect_id = -1`
7. 编译单元需要 C++23（组件已 `PUBLIC cxx_std_23`）

## 作者

- 虾哥 Terrence (terrence@tenclass.com)
