#ifndef _AT_UART_H_
#define _AT_UART_H_

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <list>
#include <cstdlib>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_pm.h>
#include <esp_log.h>
#include <esp_sleep.h>

// UART事件定义
#define AT_EVENT_DATA_AVAILABLE BIT1
#define AT_EVENT_COMMAND_DONE   BIT2
#define AT_EVENT_COMMAND_ERROR  BIT3
#define AT_EVENT_BUFFER_FULL    BIT4
#define AT_EVENT_FIFO_OVF       BIT5
#define AT_EVENT_BREAK          BIT6
#define AT_EVENT_RI_PIN_INT     BIT7  // RI pin interrupt event
#define AT_EVENT_UNKNOWN        BIT8

// 默认配置
#define UART_NUM                UART_NUM_1

// AT命令参数值结构
struct AtArgumentValue {
    enum class Type { String, Int, Double };
    Type type;
    std::string string_value;
    int int_value;
    double double_value;
    
    std::string ToString() const {
        switch (type) {
            case Type::String:
                return "\"" + string_value + "\"";
            case Type::Int:
                return std::to_string(int_value);
            case Type::Double:
                return std::to_string(double_value);
            default:
                return "";
        }
    }
};

// 数据接收回调函数类型
typedef std::function<void(const std::string& command, const std::vector<AtArgumentValue>& arguments)> UrcCallback;

class AtUart {
public:
    // 构造函数
    AtUart(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin = GPIO_NUM_NC, gpio_num_t ri_pin = GPIO_NUM_NC);
    ~AtUart();

    // 初始化和配置
    void Initialize();
    
    // 波特率管理
    bool SetBaudRate(int new_baud_rate, int timeout_ms = -1);
    int GetBaudRate() const { return baud_rate_; }
    
    // 数据发送
    bool SendCommand(const std::string& command, size_t timeout_ms = 1000, bool add_crlf = true);
    bool SendCommandWithData(const std::string& command, size_t timeout_ms = 1000, bool add_crlf = true, const char* data = nullptr, size_t data_length = 0);
    std::string GetResponse() const;
    int GetCmeErrorCode() const { return cme_error_code_; }
    
    // 回调管理
    std::list<UrcCallback>::iterator RegisterUrcCallback(UrcCallback callback);
    void UnregisterUrcCallback(std::list<UrcCallback>::iterator iterator);
    
    // 控制接口
    void SetDtrPin(bool high);
    bool GetDtrPin() const { return dtr_pin_state_; }
    bool IsInitialized() const { return initialized_; }
    void SetDebug(bool enable);

    std::string EncodeHex(const std::string& data);
    std::string DecodeHex(const std::string& data);
    void EncodeHexAppend(std::string& dest, const char* data, size_t length);
    void DecodeHexAppend(std::string& dest, const char* data, size_t length);

private:
    // 配置参数
    gpio_num_t tx_pin_;
    gpio_num_t rx_pin_;
    gpio_num_t dtr_pin_;
    gpio_num_t ri_pin_;
    uart_port_t uart_num_;
    int baud_rate_;
    bool initialized_;
    bool dtr_pin_state_;  // 记录DTR pin的当前状态
    bool debug_ = false;  // Debug mode flag
    int cme_error_code_ = 0;
    std::string response_;
    bool wait_for_response_ = false;
    std::mutex command_mutex_;
    mutable std::mutex mutex_;
    std::mutex dtr_mutex_;
    esp_pm_lock_handle_t pm_lock_;
    esp_pm_lock_handle_t ri_pm_lock_;  // RI pin PM lock
    bool ri_pm_lock_acquired_;  // Track RI PM lock state
    
    // FreeRTOS 对象
    TaskHandle_t event_task_handle_ = nullptr;
    TaskHandle_t receive_task_handle_ = nullptr;
    QueueHandle_t event_queue_handle_;
    EventGroupHandle_t event_group_handle_;
    
    std::string rx_buffer_;
    
    // 回调函数
    std::list<UrcCallback> urc_callbacks_;
    
    // 内部方法
    void EventTask();
    void ReceiveTask();
    bool ParseResponse();
    bool DetectBaudRate(int timeout_ms = -1);
    // 处理 URC
    void HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments);
    bool SendData(const char* data, size_t length);
    
    // RI pin ISR handler
    static void IRAM_ATTR RiPinIsrHandler(void* arg);

    friend class DtrGuard;
};

/**
 * RAII guard for modem DTR pin management
 * Automatically activates modem (DTR=false) on construction
 * and deactivates modem (DTR=true) on destruction
 */
class DtrGuard {
public:
    explicit DtrGuard(AtUart* at_uart)
        : at_uart_(at_uart), active_(false) {
        if (at_uart_ && at_uart_->GetDtrPin()) {
            // Lock DTR mutex to ensure exclusive access
            lock_ = std::unique_lock<std::mutex>(at_uart_->dtr_mutex_);
            // Acquire PM lock before activating modem
            esp_pm_lock_acquire(at_uart_->pm_lock_);
            at_uart_->SetDtrPin(false);
            active_ = true;
        }
    }

    ~DtrGuard() {
        if (at_uart_ && active_) {
            at_uart_->SetDtrPin(true);
            // Release PM lock after deactivating modem
            esp_pm_lock_release(at_uart_->pm_lock_);
        }
        // lock_ will be automatically unlocked when it goes out of scope
    }

    // Non-copyable
    DtrGuard(const DtrGuard&) = delete;
    DtrGuard& operator=(const DtrGuard&) = delete;

    // Non-movable
    DtrGuard(DtrGuard&&) = delete;
    DtrGuard& operator=(DtrGuard&&) = delete;

private:
    AtUart* at_uart_;
    bool active_;
    std::unique_lock<std::mutex> lock_;
};

#endif // _AT_UART_H_
