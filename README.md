# ML307 Series Cat.1 AT Modem (v3.0)

这是一个适用于 ML307R / ML307A Cat.1 模组的组件。
本项目最初为 https://github.com/78/xiaozhi-esp32 项目创建。

## 🆕 版本 3.0 新特性

- **自动模组检测**: 自动识别 ML307 和 EC801E 模组
- **统一接口**: 通过 `NetworkInterface` 基类提供一致的API
- **智能内存管理**: 使用 `shared_ptr<AtUart>` 确保内存安全
- **简化的API**: 更加直观和易用的接口设计

## 功能特性

- AT 命令
- MQTT / MQTTS
- HTTP / HTTPS
- TCP / SSL TCP
- UDP
- WebSocket
- 自动模组检测和初始化

## 支持的模组

- ML307R
- ML307A
- EC801E (新增)

## 快速开始

### 基础用法

```cpp
#include "esp_log.h"
#include "at_modem.h"

static const char *TAG = "ML307_DEMO";

extern "C" void app_main(void) {
    // 自动检测并初始化模组
    auto modem = AtModem::Detect(GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15, 921600);
    
    if (!modem) {
        ESP_LOGE(TAG, "模组检测失败");
        return;
    }
    
    // 设置网络状态回调
    modem->OnNetworkStateChanged([](bool ready) {
        ESP_LOGI(TAG, "网络状态: %s", ready ? "已连接" : "已断开");
    });
    
    // 等待网络就绪
    NetworkStatus status = modem->WaitForNetworkReady(30000);
    if (status != NetworkStatus::Ready) {
        ESP_LOGE(TAG, "网络连接失败");
        return;
    }
    
    // 打印模组信息
    ESP_LOGI(TAG, "模组版本: %s", modem->GetModuleRevision().c_str());
    ESP_LOGI(TAG, "IMEI: %s", modem->GetImei().c_str());
    ESP_LOGI(TAG, "ICCID: %s", modem->GetIccid().c_str());
    ESP_LOGI(TAG, "运营商: %s", modem->GetCarrierName().c_str());
    ESP_LOGI(TAG, "信号强度: %d", modem->GetCsq());
}
```

### HTTP 客户端

```cpp
void TestHttp(std::unique_ptr<AtModem>& modem) {
    ESP_LOGI(TAG, "开始 HTTP 测试");

    // 创建 HTTP 客户端
    Http* http = modem->CreateHttp(0);
    
    // 设置请求头
    http->SetHeader("User-Agent", "Xiaozhi/3.0.0");
    http->SetTimeout(10000);
    
    // 发送 GET 请求
    if (http->Open("GET", "https://httpbin.org/json")) {
        ESP_LOGI(TAG, "HTTP 状态码: %d", http->GetStatusCode());
        ESP_LOGI(TAG, "响应内容长度: %zu bytes", http->GetBodyLength());
        
        // 读取响应内容
        std::string response = http->ReadAll();
        ESP_LOGI(TAG, "响应内容: %s", response.c_str());
        
        http->Close();
    } else {
        ESP_LOGE(TAG, "HTTP 请求失败");
    }
    
    delete http;
}
```

### MQTT 客户端

```cpp
void TestMqtt(std::unique_ptr<AtModem>& modem) {
    ESP_LOGI(TAG, "开始 MQTT 测试");

    // 创建 MQTT 客户端
    Mqtt* mqtt = modem->CreateMqtt(0);
    
    // 设置回调函数
    mqtt->OnConnected([]() {
        ESP_LOGI(TAG, "MQTT 连接成功");
    });
    
    mqtt->OnDisconnected([]() {
        ESP_LOGI(TAG, "MQTT 连接断开");
    });
    
    mqtt->OnMessage([](const std::string& topic, const std::string& payload) {
        ESP_LOGI(TAG, "收到消息 [%s]: %s", topic.c_str(), payload.c_str());
    });
    
    // 连接到 MQTT 代理
    if (mqtt->Connect("broker.emqx.io", 1883, "esp32_client", "", "")) {
        // 订阅主题
        mqtt->Subscribe("test/esp32/message");
        
        // 发布消息
        mqtt->Publish("test/esp32/hello", "Hello from ESP32!");
        
        // 等待一段时间接收消息
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        mqtt->Disconnect();
    } else {
        ESP_LOGE(TAG, "MQTT 连接失败");
    }
    
    delete mqtt;
}
```

### WebSocket 客户端

```cpp
void TestWebSocket(std::unique_ptr<AtModem>& modem) {
    ESP_LOGI(TAG, "开始 WebSocket 测试");

    // 创建 WebSocket 客户端
    WebSocket* ws = modem->CreateWebSocket(0);
    
    // 设置请求头
    ws->SetHeader("Protocol-Version", "3");
    
    // 设置回调函数
    ws->OnConnected([]() {
        ESP_LOGI(TAG, "WebSocket 连接成功");
    });
    
    ws->OnData([](const char* data, size_t length, bool binary) {
        ESP_LOGI(TAG, "收到数据: %.*s", (int)length, data);
    });
    
    ws->OnDisconnected([]() {
        ESP_LOGI(TAG, "WebSocket 连接断开");
    });
    
    ws->OnError([](int error) {
        ESP_LOGE(TAG, "WebSocket 错误: %d", error);
    });
    
    // 连接到 WebSocket 服务器
    if (ws->Connect("wss://echo.websocket.org/")) {
        // 发送消息
        for (int i = 0; i < 5; i++) {
            std::string message = "{\"type\": \"ping\", \"id\": " + std::to_string(i) + "}";
            ws->Send(message);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        
        ws->Close();
    } else {
        ESP_LOGE(TAG, "WebSocket 连接失败");
    }
    
    delete ws;
}
```

### TCP 客户端

```cpp
void TestTcp(std::unique_ptr<AtModem>& modem) {
    ESP_LOGI(TAG, "开始 TCP 测试");

    // 创建 TCP 客户端
    Tcp* tcp = modem->CreateTcp(0);
    
    // 设置数据接收回调
    tcp->OnStream([](const std::string& data) {
        ESP_LOGI(TAG, "TCP 接收数据: %s", data.c_str());
    });
    
    // 设置断开连接回调
    tcp->OnDisconnected([]() {
        ESP_LOGI(TAG, "TCP 连接已断开");
    });
    
    if (tcp->Connect("httpbin.org", 80)) {
        // 发送 HTTP 请求
        std::string request = "GET /ip HTTP/1.1\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n";
        int sent = tcp->Send(request);
        ESP_LOGI(TAG, "TCP 发送了 %d 字节", sent);
        
        // 等待接收响应（通过回调处理）
        vTaskDelay(pdMS_TO_TICKS(3000));
        
        tcp->Disconnect();
    } else {
        ESP_LOGE(TAG, "TCP 连接失败");
    }
    
    delete tcp;
}
```

## 高级用法

### 直接访问 AtUart

```cpp
void DirectAtCommand(std::unique_ptr<AtModem>& modem) {
    // 获取共享的 AtUart 实例
    auto uart = modem->GetAtUart();
    
    // 发送自定义 AT 命令
    if (uart->SendCommand("AT+CSQ", 1000)) {
        std::string response = uart->GetResponse();
        ESP_LOGI(TAG, "信号强度查询结果: %s", response.c_str());
    }
    
    // 可以在多个地方安全地持有 uart 引用
    std::shared_ptr<AtUart> my_uart = modem->GetAtUart();
    // my_uart 可以在其他线程或对象中安全使用
}
```

### 网络状态监控

```cpp
void MonitorNetwork(std::unique_ptr<AtModem>& modem) {
    // 监控网络状态变化
    modem->OnNetworkStateChanged([&modem](bool ready) {
        if (ready) {
            ESP_LOGI(TAG, "网络已就绪");
            ESP_LOGI(TAG, "信号强度: %d", modem->GetCsq());
            
            auto reg_state = modem->GetRegistrationState();
            ESP_LOGI(TAG, "注册状态: %s", reg_state.ToString().c_str());
        } else {
            ESP_LOGE(TAG, "网络连接丢失");
        }
    });
    
    // 检查网络状态
    if (modem->network_ready()) {
        ESP_LOGI(TAG, "当前网络状态: 已连接");
    } else {
        ESP_LOGI(TAG, "当前网络状态: 未连接");
    }
}
```

## 错误处理

```cpp
void HandleErrors(std::unique_ptr<AtModem>& modem) {
    // 等待网络就绪，处理各种错误情况
    NetworkStatus status = modem->WaitForNetworkReady(30000);
    
    switch (status) {
        case NetworkStatus::Ready:
            ESP_LOGI(TAG, "网络连接成功");
            break;
        case NetworkStatus::ErrorInsertPin:
            ESP_LOGE(TAG, "SIM 卡未插入或 PIN 码错误");
            break;
        case NetworkStatus::ErrorRegistrationDenied:
            ESP_LOGE(TAG, "网络注册被拒绝");
            break;
        case NetworkStatus::ErrorTimeout:
            ESP_LOGE(TAG, "网络连接超时");
            break;
        default:
            ESP_LOGE(TAG, "未知网络错误");
            break;
    }
}
```

## 迁移指南 (v2.x → v3.0)

### 旧版本 (v2.x)

```cpp
// 旧方式：需要明确指定模组类型和GPIO引脚
Ml307AtModem modem(GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15);
NetworkStatus status = modem.WaitForNetworkReady();

Ml307Http http(modem);
http.Open("GET", "https://example.com");
```

### 新版本 (v3.0)

```cpp
// 新方式：自动检测模组类型
auto modem = AtModem::Detect(GPIO_NUM_13, GPIO_NUM_14, GPIO_NUM_15);
NetworkStatus status = modem->WaitForNetworkReady();

Http* http = modem->CreateHttp(0);
http->Open("GET", "https://example.com");
delete http;
```

## 架构优势

1. **自动化**: 无需手动指定模组类型，提高代码通用性
2. **统一接口**: 不同模组使用相同的API
3. **代码复用**: 避免重复实现相同功能
4. **易于维护**: 公共逻辑集中管理
5. **扩展性**: 便于添加新的模组类型支持
6. **内存安全**: `shared_ptr<AtUart>` 提供自动内存管理
7. **线程安全**: 支持多线程安全访问

## 注意事项

1. 构造函数已变化，现在使用 `AtModem::Detect()` 方法
2. 协议客户端需要通过 `CreateXxx()` 方法创建
3. 记得在使用完协议客户端后调用 `delete` 释放内存
4. 网络状态通过回调函数异步通知
5. `GetAtUart()` 返回 `shared_ptr<AtUart>`，支持安全共享

## 作者

- 虾哥 Terrence (terrence@tenclass.com)
