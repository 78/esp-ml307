#ifndef AT_ERROR_H
#define AT_ERROR_H

#include "network_error.h"

#include <expected>
#include <string>

#include <esp_err.h>

enum class AtErrc {
    Timeout,
    CommandError,
    CmeError,
    TransmitFailed,
    NotInitialized,
};

struct AtError {
    AtErrc code = AtErrc::CommandError;
    int cme = 0;
    esp_err_t esp = ESP_OK;

    NetworkError ToNetworkError() const;
    const char* Message() const;
    std::string ToString() const;
};

using AtResult = std::expected<void, AtError>;

template <typename T>
using AtValue = std::expected<T, AtError>;

#endif  // AT_ERROR_H
