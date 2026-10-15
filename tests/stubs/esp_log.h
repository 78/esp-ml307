#pragma once

// Consumes the arguments so variables used only for logging stay "used".
template <typename... Args>
inline void esp_log_stub(const char*, const char*, const Args&...) {}

#define ESP_LOGE(tag, format, ...) esp_log_stub(tag, format, ##__VA_ARGS__)
#define ESP_LOGW(tag, format, ...) esp_log_stub(tag, format, ##__VA_ARGS__)
#define ESP_LOGI(tag, format, ...) esp_log_stub(tag, format, ##__VA_ARGS__)
#define ESP_LOGD(tag, format, ...) esp_log_stub(tag, format, ##__VA_ARGS__)
