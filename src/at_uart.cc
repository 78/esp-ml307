#include "at_uart.h"
#include <esp_log.h>
#include <esp_err.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <sstream>

#define TAG "AtUart"

// RX data item for queue - stores the buffer pointer for later return
struct RxDataItem {
    UartUhci::RxBuffer* buffer;
    size_t size;
};

// AtUart 构造函数实现
AtUart::AtUart(gpio_num_t tx_pin, gpio_num_t rx_pin, gpio_num_t dtr_pin, gpio_num_t ri_pin)
    : tx_pin_(tx_pin), rx_pin_(rx_pin), dtr_pin_(dtr_pin), ri_pin_(ri_pin), uart_num_(UART_NUM),
      baud_rate_(115200), initialized_(false), dtr_pin_state_(false),
      pm_lock_(nullptr), ri_pm_lock_(nullptr), ri_pm_lock_acquired_(false),
      receive_task_handle_(nullptr), rx_data_queue_(nullptr), event_group_handle_(nullptr) {
    // Create power management lock for DTR operations
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "at_uart_pm_lock", &pm_lock_);
    // Create power management lock for RI pin operations
    if (ri_pin_ != GPIO_NUM_NC) {
        esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "at_uart_ri_pm_lock", &ri_pm_lock_);
    }
}

AtUart::~AtUart() {
    if (receive_task_handle_) {
        vTaskDelete(receive_task_handle_);
    }
    if (event_task_handle_) {
        vTaskDelete(event_task_handle_);
    }
    if (event_group_handle_) {
        vEventGroupDelete(event_group_handle_);
    }
    if (rx_data_queue_) {
        // Clean up any remaining items in queue - return buffers to pool
        RxDataItem item;
        while (xQueueReceive(rx_data_queue_, &item, 0) == pdTRUE) {
            if (item.buffer) {
                uart_uhci_.ReturnBuffer(item.buffer);
            }
        }
        vQueueDelete(rx_data_queue_);
    }
    if (initialized_) {
        // Remove RI pin ISR handler if configured
        if (ri_pin_ != GPIO_NUM_NC) {
            gpio_isr_handler_remove(ri_pin_);
        }
        // Deinitialize UHCI
        uart_uhci_.Deinit();
    }
    if (ri_pm_lock_) {
        if (ri_pm_lock_acquired_) {
            esp_pm_lock_release(ri_pm_lock_);
        }
        esp_pm_lock_delete(ri_pm_lock_);
    }
    if (pm_lock_) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void AtUart::Initialize() {
    if (initialized_) {
        return;
    }
    
    event_group_handle_ = xEventGroupCreate();
    if (!event_group_handle_) {
        ESP_LOGE(TAG, "创建事件组失败");
        return;
    }

    // Create RX data queue
    rx_data_queue_ = xQueueCreate(16, sizeof(RxDataItem));
    if (!rx_data_queue_) {
        ESP_LOGE(TAG, "创建RX数据队列失败");
        return;
    }

    // Configure UART parameters (no driver install, UHCI will take over)
    uart_config_t uart_config = {};
    uart_config.baud_rate = baud_rate_;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.source_clk = UART_SCLK_DEFAULT;
    
    ESP_ERROR_CHECK(uart_param_config(uart_num_, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(uart_num_, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    // Enable pull-up on RX pin
    gpio_set_pull_mode(rx_pin_, GPIO_PULLUP_ONLY);
    
    // Initialize UHCI DMA controller
    UartUhci::Config uhci_cfg = {
        .uart_port = uart_num_,
        .dma_burst_size = 32,
        .rx_pool = {
            .buffer_count = AT_UART_RX_BUFFER_COUNT,
            .buffer_size = AT_UART_RX_BUFFER_SIZE,
        },
    };
    
    esp_err_t ret = uart_uhci_.Init(uhci_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UHCI初始化失败: %s", esp_err_to_name(ret));
        return;
    }
    
    // Register DMA RX callback
    uart_uhci_.SetRxCallback(DmaRxCallback, this);
    
    // Register DMA overflow callback
    uart_uhci_.SetOverflowCallback(DmaOverflowCallback, this);
    
    // Start DMA receive
    ret = uart_uhci_.StartReceive();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动DMA接收失败: %s", esp_err_to_name(ret));
        return;
    }
    
    if (dtr_pin_ != GPIO_NUM_NC) {
        gpio_config_t config = {};
        config.pin_bit_mask = (1ULL << dtr_pin_);
        config.mode = GPIO_MODE_OUTPUT;
        config.pull_up_en = GPIO_PULLUP_DISABLE;
        config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        config.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&config);
        gpio_set_level(dtr_pin_, 0);
        dtr_pin_state_ = false;  // 记录初始状态为低电平
    }

    // Configure RI pin as input with interrupt
    if (ri_pin_ != GPIO_NUM_NC) {
        gpio_config_t ri_config = {};
        ri_config.pin_bit_mask = (1ULL << ri_pin_);
        ri_config.mode = GPIO_MODE_INPUT;
        ri_config.pull_up_en = GPIO_PULLUP_ENABLE;  // Enable pull-up for RI pin
        ri_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
        ri_config.intr_type = GPIO_INTR_LOW_LEVEL;  // Trigger on falling edge (low level)
        gpio_config(&ri_config);

        gpio_wakeup_enable(ri_pin_, GPIO_INTR_LOW_LEVEL);

        // Add ISR handler for RI pin
        gpio_isr_handler_add(ri_pin_, RiPinIsrHandler, this);
    }

    // ReceiveTask: high priority, only handles DMA data reception
    xTaskCreate([](void* arg) {
        auto at_uart = (AtUart*)arg;
        at_uart->ReceiveTask();
        vTaskDelete(NULL);
    }, "modem_receive", 1024, this, configMAX_PRIORITIES - 2, &receive_task_handle_);

    // EventTask: lower priority, handles parsing and URC callbacks
    xTaskCreate([](void* arg) {
        auto at_uart = (AtUart*)arg;
        at_uart->EventTask();
        vTaskDelete(NULL);
    }, "modem_event", 2048 * 3, this, configMAX_PRIORITIES - 3, &event_task_handle_);

    initialized_ = true;
}

// DMA RX callback (called from ISR context)
bool IRAM_ATTR AtUart::DmaRxCallback(const UartUhci::RxEventData& data, void* user_data) {
    AtUart* self = static_cast<AtUart*>(user_data);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    if (data.buffer && data.recv_size > 0) {
        // Send buffer pointer to queue for processing in task context
        // The buffer will be returned by ReceiveTask after processing
        RxDataItem item;
        item.buffer = data.buffer;
        item.size = data.recv_size;
        
        if (xQueueSendFromISR(self->rx_data_queue_, &item, &xHigherPriorityTaskWoken) != pdTRUE) {
            // Queue full, return buffer immediately
            ESP_DRAM_LOGW("AtUart", "RX queue full, dropping %u bytes", data.recv_size);
            self->uart_uhci_.ReturnBuffer(data.buffer);
        }
    } else if (data.buffer) {
        // Empty buffer, return immediately
        ESP_DRAM_LOGW("AtUart", "Empty buffer received, size=%u", data.recv_size);
        self->uart_uhci_.ReturnBuffer(data.buffer);
    }
    
    return xHigherPriorityTaskWoken == pdTRUE;
}

// DMA overflow callback (called from ISR context when buffer exhaustion detected)
bool IRAM_ATTR AtUart::DmaOverflowCallback(void* user_data) {
    AtUart* self = static_cast<AtUart*>(user_data);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    
    // Signal overflow event to ReceiveTask
    xEventGroupSetBitsFromISR(self->event_group_handle_, AT_EVENT_FIFO_OVERFLOW, &xHigherPriorityTaskWoken);
    
    return xHigherPriorityTaskWoken == pdTRUE;
}

void AtUart::ReceiveTask() {
    // This task only handles data reception from DMA queue
    // It runs at high priority to ensure timely buffer return to UHCI pool
    RxDataItem item;
    while (true) {
        // Block waiting for data from DMA queue
        if (xQueueReceive(rx_data_queue_, &item, portMAX_DELAY) == pdTRUE) {
            if (item.buffer && item.size > 0) {
                // Append to rx_buffer_ with lock protection
                {
                    std::lock_guard<std::mutex> lock(rx_buffer_mutex_);
                    rx_buffer_.append(reinterpret_cast<char*>(item.buffer->data), item.size);
                }
                // Return buffer to UHCI pool immediately
                uart_uhci_.ReturnBuffer(item.buffer);
                // Notify EventTask to parse response
                xEventGroupSetBits(event_group_handle_, AT_EVENT_PARSE_NEEDED);
            }
        }
    }
}

void AtUart::EventTask() {
    // This task handles parsing and event processing
    // It runs at lower priority so ReceiveTask can quickly return DMA buffers
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_handle_, 
            AT_EVENT_PARSE_NEEDED | AT_EVENT_RI_PIN_INT | AT_EVENT_FIFO_OVERFLOW,
            pdTRUE, pdFALSE, portMAX_DELAY);
        
        if (bits & AT_EVENT_PARSE_NEEDED) {
            // Parse all available responses
            while (ParseResponse()) {}
        }
        
        if (bits & AT_EVENT_FIFO_OVERFLOW) {
            // DMA buffer exhaustion detected - notify upper layer via URC
            ESP_LOGW(TAG, "DMA buffer overflow detected, notifying upper layer");
            HandleUrc("FIFO_OVERFLOW", {});
        }

        if (ri_pin_ != GPIO_NUM_NC) {
            if (bits & AT_EVENT_RI_PIN_INT) {
                // RI pin went low - acquire PM lock to prevent sleep
                if (!ri_pm_lock_acquired_) {
                    esp_pm_lock_acquire(ri_pm_lock_);
                    ri_pm_lock_acquired_ = true;
                    ESP_LOGD(TAG, "RI pin went low, PM lock acquired");
                }
            } else {
                // Release RI PM lock when data is available (modem has data to send)
                if (ri_pm_lock_acquired_) {
                    esp_pm_lock_release(ri_pm_lock_);
                    ri_pm_lock_acquired_ = false;
                    gpio_intr_enable(ri_pin_);
                    ESP_LOGD(TAG, "Data available, RI PM lock released");
                }
            }
        }
    }
}

static bool is_number(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit) && s.length() < 10;
}

bool AtUart::ParseResponse() {
    std::string command, values;
    std::string::size_type end_pos;
    
    // Lock rx_buffer_ for the duration of parsing
    {
        std::lock_guard<std::mutex> lock(rx_buffer_mutex_);
        
        if (rx_buffer_.empty()) {
            return false;
        }
        
        if (wait_for_response_ && rx_buffer_[0] == '>') {
            rx_buffer_.erase(0, 1);
            xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_DONE);
            return true;
        }

        end_pos = rx_buffer_.find("\r\n");
        if (end_pos == std::string::npos) {
            // FIXME: for +MHTTPURC: "ind", missing newline
            if (rx_buffer_.size() >= 16 && memcmp(rx_buffer_.c_str(), "+MHTTPURC: \"ind\"", 16) == 0) {
                // Find the end of this line and add \r\n if missing
                auto next_plus = rx_buffer_.find("+", 1);
                if (next_plus != std::string::npos) {
                    // Insert \r\n before the next + command
                    rx_buffer_.insert(next_plus, "\r\n");
                } else {
                    // Append \r\n at the end
                    rx_buffer_.append("\r\n");
                }
                end_pos = rx_buffer_.find("\r\n");
            } else {
                return false;
            }
        }

        // Ignore empty lines
        if (end_pos == 0) {
            rx_buffer_.erase(0, 2);
            return true;
        }

        if (debug_) {
            ESP_LOGI(TAG, "<< %.64s (%u bytes) [%02x%02x%02x]", rx_buffer_.substr(0, end_pos).c_str(), end_pos,
                rx_buffer_[0], rx_buffer_[1], rx_buffer_[2]);
        }

        // Parse "+CME ERROR: 123,456,789"
        if (rx_buffer_[0] == '+') {
            auto pos = rx_buffer_.find(": ");
            if (pos == std::string::npos || pos > end_pos) {
                command = rx_buffer_.substr(1, end_pos - 1);
            } else {
                command = rx_buffer_.substr(1, pos - 1);
                values = rx_buffer_.substr(pos + 2, end_pos - pos - 2);
            }
            rx_buffer_.erase(0, end_pos + 2);
            // Will call HandleUrc after releasing lock
        } else if (rx_buffer_.size() >= 4 && rx_buffer_[0] == 'O' && rx_buffer_[1] == 'K' && rx_buffer_[2] == '\r' && rx_buffer_[3] == '\n') {
            rx_buffer_.erase(0, 4);
            xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_DONE);
            return true;
        } else if (rx_buffer_.size() >= 7 && rx_buffer_[0] == 'E' && rx_buffer_[1] == 'R' && rx_buffer_[2] == 'R' && rx_buffer_[3] == 'O' && rx_buffer_[4] == 'R' && rx_buffer_[5] == '\r' && rx_buffer_[6] == '\n') {
            rx_buffer_.erase(0, 7);
            xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_ERROR);
            return true;
        } else if (rx_buffer_[0] == 0xE0) { // 4G wake up MCU, just ignore
            rx_buffer_.erase(0, end_pos + 2);
            return true;
        } else {
            std::lock_guard<std::mutex> response_lock(mutex_);
            response_ = rx_buffer_.substr(0, end_pos);
            rx_buffer_.erase(0, end_pos + 2);
            return true;
        }
    }
    
    // Handle URC outside the rx_buffer_ lock to avoid blocking ReceiveTask
    if (!command.empty()) {
        // Parse "string", int, int, ... into AtArgumentValue
        std::vector<AtArgumentValue> arguments;
        std::istringstream iss(values);
        std::string item;
        while (std::getline(iss, item, ',')) {
            AtArgumentValue argument;
            if (item.front() == '"') {
                argument.type = AtArgumentValue::Type::String;
                argument.string_value = item.substr(1, item.size() - 2);
            } else if (item.find(".") != std::string::npos) {
                argument.type = AtArgumentValue::Type::Double;
                argument.double_value = std::stod(item);
            } else if (is_number(item)) {
                argument.type = AtArgumentValue::Type::Int;
                argument.int_value = std::stoi(item);
                argument.string_value = std::move(item);
            } else {
                argument.type = AtArgumentValue::Type::String;
                argument.string_value = std::move(item);
            }
            arguments.push_back(argument);
        }

        HandleUrc(command, arguments);
        return true;
    }
    
    return false;
}

void AtUart::HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments) {
    if (command == "CME ERROR") {
        cme_error_code_ = arguments[0].int_value;
        xEventGroupSetBits(event_group_handle_, AT_EVENT_COMMAND_ERROR);
        return;
    }

    std::lock_guard<std::mutex> lock(urc_mutex_);
    for (auto& callback : urc_callbacks_) {
        callback(command, arguments);
    }
}

bool AtUart::DetectBaudRate(int timeout_ms) {
    int baud_rates[] = {115200, 921600, 460800, 230400, 57600, 38400, 19200, 9600};
    TickType_t start_time = xTaskGetTickCount();
    TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    
    while (true) {
        ESP_LOGI(TAG, "Detecting baud rate...");
        for (size_t i = 0; i < sizeof(baud_rates) / sizeof(baud_rates[0]); i++) {
            int rate = baud_rates[i];
            uart_set_baudrate(uart_num_, rate);
            if (SendCommand("AT", 20)) {
                ESP_LOGI(TAG, "Detected baud rate: %d", rate);
                baud_rate_ = rate;
                return true;
            }
        }
        
        // Check timeout before delay if specified
        if (timeout_ms != -1) {
            TickType_t elapsed = xTaskGetTickCount() - start_time;
            if (elapsed >= timeout_ticks) {
                ESP_LOGE(TAG, "Baud rate detection timeout");
                return false;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

bool AtUart::SetBaudRate(int new_baud_rate, int timeout_ms) {
    if (!DetectBaudRate(timeout_ms)) {
        ESP_LOGE(TAG, "Failed to detect baud rate");
        return false;
    }
    if (new_baud_rate == baud_rate_) {
        return true;
    }
    // Set new baud rate
    if (!SendCommand(std::string("AT+IPR=") + std::to_string(new_baud_rate))) {
        ESP_LOGI(TAG, "Failed to set baud rate to %d", new_baud_rate);
        return false;
    }
    uart_set_baudrate(uart_num_, new_baud_rate);
    baud_rate_ = new_baud_rate;
    ESP_LOGI(TAG, "Set baud rate to %d", new_baud_rate);
    return true;
}

bool AtUart::SendData(const char* data, size_t length) {
    if (!initialized_) {
        ESP_LOGE(TAG, "UART未初始化");
        return false;
    }
    
    esp_err_t ret = uart_uhci_.Transmit(reinterpret_cast<const uint8_t*>(data), length);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UHCI transmit failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool AtUart::SendCommandWithData(const std::string& command, size_t timeout_ms, bool add_crlf, const char* data, size_t data_length) {
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (debug_) {
        ESP_LOGI(TAG, ">> %.64s (%u bytes)", command.data(), command.length());
    }

    xEventGroupClearBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR);
    wait_for_response_ = true;
    cme_error_code_ = 0;
    {
        std::lock_guard<std::mutex> response_lock(mutex_);
        response_.clear();
    }

    if (add_crlf) {
        if (!SendData((command + "\r\n").data(), command.length() + 2)) {
            return false;
        }
    } else {
        if (!SendData(command.data(), command.length())) {
            return false;
        }
    }
    if (timeout_ms > 0) {
        auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        wait_for_response_ = false;
        if (!(bits & AT_EVENT_COMMAND_DONE)) {
            return false;
        }
    } else {
        wait_for_response_ = false;
    }

    if (data && data_length > 0) {
        wait_for_response_ = true;
        if (!SendData(data, data_length)) {
            return false;
        }
        auto bits = xEventGroupWaitBits(event_group_handle_, AT_EVENT_COMMAND_DONE | AT_EVENT_COMMAND_ERROR, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        wait_for_response_ = false;
        if (!(bits & AT_EVENT_COMMAND_DONE)) {
            return false;
        }
    }
    return true;
}

bool AtUart::SendCommand(const std::string& command, size_t timeout_ms, bool add_crlf) {
    return SendCommandWithData(command, timeout_ms, add_crlf, nullptr, 0);
}

std::string AtUart::GetResponse() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return response_;
}

std::list<UrcCallback>::iterator AtUart::RegisterUrcCallback(UrcCallback callback) {
    std::lock_guard<std::mutex> lock(urc_mutex_);
    return urc_callbacks_.insert(urc_callbacks_.end(), callback);
}

void AtUart::UnregisterUrcCallback(std::list<UrcCallback>::iterator iterator) {
    std::lock_guard<std::mutex> lock(urc_mutex_);
    urc_callbacks_.erase(iterator);
}

void AtUart::SetDtrPin(bool high) {
    if (dtr_pin_ != GPIO_NUM_NC) {
        if (debug_) {
            ESP_LOGI(TAG, "Set DTR pin %d to %d", dtr_pin_, high ? 1 : 0);
        }
        gpio_set_level(dtr_pin_, high ? 1 : 0);
        dtr_pin_state_ = high;  // 记录DTR pin的状态
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static const char hex_chars[] = "0123456789ABCDEF";
// 辅助函数，将单个十六进制字符转换为对应的数值
inline uint8_t CharToHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0;  // 对于无效输入，返回0
}

void AtUart::EncodeHexAppend(std::string& dest, const char* data, size_t length) {
    dest.reserve(dest.size() + length * 2 + 4);  // 预分配空间，多分配4个字节用于\r\n\0
    for (size_t i = 0; i < length; i++) {
        dest.push_back(hex_chars[(data[i] & 0xF0) >> 4]);
        dest.push_back(hex_chars[data[i] & 0x0F]);
    }
}

void AtUart::DecodeHexAppend(std::string& dest, const char* data, size_t length) {
    dest.reserve(dest.size() + length / 2 + 4);  // 预分配空间，多分配4个字节用于\r\n\0
    for (size_t i = 0; i < length; i += 2) {
        char byte = (CharToHex(data[i]) << 4) | CharToHex(data[i + 1]);
        dest.push_back(byte);
    }
}

std::string AtUart::EncodeHex(const std::string& data) {
    std::string encoded;
    EncodeHexAppend(encoded, data.c_str(), data.size());
    return encoded;
}

std::string AtUart::DecodeHex(const std::string& data) {
    std::string decoded;
    DecodeHexAppend(decoded, data.c_str(), data.size());
    return decoded;
}

void AtUart::SetDebug(bool enable) {
    debug_ = enable;
}

// RI pin ISR handler (runs in IRAM)
void IRAM_ATTR AtUart::RiPinIsrHandler(void* arg) {
    AtUart* at_uart = static_cast<AtUart*>(arg);
    // Disable interrupt
    gpio_intr_disable(at_uart->ri_pin_);
    // Notify the task to handle the interrupt
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xEventGroupSetBitsFromISR(at_uart->event_group_handle_, AT_EVENT_RI_PIN_INT, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
