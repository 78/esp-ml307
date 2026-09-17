#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#define IRAM_ATTR
#define ESP_LOGE(...) ((void)0)
using BaseType_t = int;
using esp_err_t = int;
constexpr int pdFALSE = 0, pdTRUE = 1;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
constexpr int ESP_OK = 0, ESP_ERR_TIMEOUT = 1, ESP_ERR_INVALID_STATE = 2;
constexpr int AT_EVENT_PARSE_NEEDED = 1;
bool in_isr = false;
uint32_t hardware_baud = 115200;
esp_err_t baud_error = ESP_OK;
esp_err_t uart_get_baudrate(int, uint32_t* baud) {
    *baud = hardware_baud;
    return baud_error;
}

struct UartUhci {
    struct RxBuffer {
        uint8_t* data;
        bool returned = false;
    };
    struct RxEventData { RxBuffer* buffer; size_t recv_size; };
    std::vector<RxBuffer*> deferred;
    int returns = 0, transmits = 0;
    uint32_t timeout = 0;
    size_t tx_size = 0;
    esp_err_t tx_result = ESP_OK;
    void DeferReturnBuffer(RxBuffer* buffer) {
        assert(in_isr && !buffer->returned);
        deferred.push_back(buffer);
    }
    void ReturnBuffer(RxBuffer* buffer) {
        assert(!in_isr && !buffer->returned);
        buffer->returned = true;
        ++returns;
    }
    void ReclaimDeferredBuffers() {
        assert(!in_isr);
        for (auto* buffer : deferred) ReturnBuffer(buffer);
        deferred.clear();
    }
    esp_err_t Transmit(const uint8_t*, size_t length, uint32_t timeout_ms) {
        ++transmits;
        tx_size = length;
        timeout = timeout_ms;
        return tx_result;
    }
};
struct RxDataItem { UartUhci::RxBuffer* buffer; size_t size; };
struct Queue { std::deque<RxDataItem> items; size_t capacity = 16; };
struct Task { uint32_t notifications = 0; };
Task task;
std::function<void()> before_wait;
struct Blocked {};
int xQueueSendFromISR(Queue* q, const RxDataItem* item, BaseType_t* woken) {
    assert(in_isr);
    if (q->items.size() == q->capacity) return pdFALSE;
    q->items.push_back(*item);
    *woken = pdTRUE;
    return pdTRUE;
}
int xQueueReceive(Queue* q, RxDataItem* item, uint32_t timeout) {
    assert(!in_isr && timeout == 0);
    if (q->items.empty()) return pdFALSE;
    *item = q->items.front();
    q->items.pop_front();
    return pdTRUE;
}
void vTaskNotifyGiveFromISR(Task* receiver, BaseType_t* woken) {
    assert(in_isr && receiver == &task);
    ++receiver->notifications;
    *woken = pdTRUE;
}
uint32_t ulTaskNotifyTake(int clear, uint32_t timeout) {
    assert(!in_isr && clear == pdTRUE && timeout == portMAX_DELAY);
    if (before_wait) {
        auto hook = std::move(before_wait);
        before_wait = {};
        hook();
    }
    auto count = task.notifications;
    task.notifications = 0;
    if (!count) throw Blocked{};
    return count;
}
void xEventGroupSetBits(int* events, int bits) { *events |= bits; }
enum class AtErrc { NotInitialized, TransmitFailed };
struct AtError { AtErrc code; int cme = 0; esp_err_t esp = 0; };
struct AtResult {
    bool ok = true;
    AtError error{AtErrc::NotInitialized};
};
struct AtUart {
    UartUhci uart_uhci_;
    Queue queue;
    Queue* rx_data_queue_ = &queue;
    Task* receive_task_handle_ = &task;
    std::string rx_buffer_;
    std::mutex rx_buffer_mutex_;
    int events = 0;
    int* event_group_handle_ = &events;
    bool initialized_ = true;
    int uart_num_ = 1;
    AtResult Fail(AtError error) { return {false, error}; }
    static bool DmaRxCallback(const UartUhci::RxEventData&, void*);
    void ReceiveTask();
    AtResult SendData(const char*, size_t);
};

// @METHODS@

bool deliver(AtUart& uart, UartUhci::RxBuffer* buffer, size_t size) {
    in_isr = true;
    bool yield = AtUart::DmaRxCallback({buffer, size}, &uart);
    in_isr = false;
    return yield;
}
void drain(AtUart& uart) {
    try { uart.ReceiveTask(); } catch (const Blocked&) {}
    assert(uart.queue.items.empty() && uart.uart_uhci_.deferred.empty());
}

int main() {
    uint8_t a[] = {'a'}, b[] = {'b'};
    // Successful delivery stays leased until the task consumes it.
    {
        AtUart uart;
        UartUhci::RxBuffer buffer{a};
        assert(deliver(uart, &buffer, 1) && !buffer.returned);
        drain(uart);
        assert(buffer.returned && uart.rx_buffer_ == "a");
        assert(uart.events == AT_EVENT_PARSE_NEEDED);
    }
    // Queue exhaustion drops data but preserves responsibility for its buffer.
    {
        AtUart uart;
        uart.queue.capacity = 1;
        UartUhci::RxBuffer first{a}, dropped{b};
        deliver(uart, &first, 1);
        assert(deliver(uart, &dropped, 1));
        assert(!dropped.returned && uart.uart_uhci_.deferred.size() == 1);
        drain(uart);
        assert(first.returned && dropped.returned && uart.rx_buffer_ == "a");
        assert(uart.uart_uhci_.returns == 2);
    }
    // Empty delivery wakes an otherwise idle task, without using a queue entry.
    {
        AtUart uart;
        UartUhci::RxBuffer empty{a};
        assert(deliver(uart, &empty, 0) && uart.queue.items.empty());
        drain(uart);
        assert(empty.returned && uart.events == 0);
    }
    // Startup ISR can precede xTaskCreate: first iteration must drain/reclaim.
    {
        AtUart uart;
        uart.receive_task_handle_ = nullptr;
        UartUhci::RxBuffer first{a}, empty{b};
        deliver(uart, &first, 1);
        deliver(uart, &empty, 0);
        assert(task.notifications == 0);
        uart.receive_task_handle_ = &task;
        drain(uart);
        assert(first.returned && empty.returned && uart.rx_buffer_ == "a");
    }
    // Reclamation/wait boundary: the ISR wakeup must survive until the wait.
    for (size_t size : {size_t{0}, size_t{1}}) {
        AtUart uart;
        UartUhci::RxBuffer buffer{a};
        before_wait = [&] { assert(deliver(uart, &buffer, size)); };
        drain(uart);
        assert(buffer.returned && uart.rx_buffer_.size() == size);
    }
    {
        AtUart uart;
        assert(!deliver(uart, nullptr, 0));
        drain(uart);
        assert(uart.uart_uhci_.returns == 0);
    }
    // Wire-time budgets cover large payloads and actual baud-detection rates.
    for (uint32_t baud : {9600u, 115200u, 3000000u}) {
        for (size_t length : {size_t{1}, size_t{32768}, size_t{UINT32_MAX}}) {
            AtUart uart;
            hardware_baud = baud;
            assert(uart.SendData("payload", length).ok);
            const uint64_t wire_ms = (uint64_t{length} * 10000 + baud - 1) / baud;
            assert(uart.uart_uhci_.timeout >= wire_ms || uart.uart_uhci_.timeout == UINT32_MAX);
            assert(uart.uart_uhci_.timeout == std::min<uint64_t>(wire_ms + 1000, UINT32_MAX));
            assert(uart.uart_uhci_.tx_size == length);
        }
    }
    {
        AtUart uart;
        assert(uart.SendData(nullptr, 0).ok && uart.uart_uhci_.transmits == 0);
        uart.initialized_ = false;
        auto result = uart.SendData("x", 1);
        assert(!result.ok && result.error.code == AtErrc::NotInitialized);
        assert(uart.uart_uhci_.transmits == 0);
    }
    for (auto error : {ESP_OK, ESP_ERR_TIMEOUT}) {
        AtUart uart;
        baud_error = error;
        hardware_baud = 0;
        auto result = uart.SendData("x", 1);
        assert(!result.ok && result.error.code == AtErrc::TransmitFailed);
        assert(result.error.esp == (error ? error : ESP_ERR_INVALID_STATE));
        assert(uart.uart_uhci_.transmits == 0);
    }
    {
        AtUart uart;
        hardware_baud = 115200;
        baud_error = ESP_OK;
        uart.uart_uhci_.tx_result = ESP_ERR_TIMEOUT;
        auto result = uart.SendData("x", 1);
        assert(!result.ok && result.error.code == AtErrc::TransmitFailed);
        assert(result.error.esp == ESP_ERR_TIMEOUT);
    }
    std::cout << "AtUart RX ownership/wakeup and TX deadline/error regressions passed\n";
}
