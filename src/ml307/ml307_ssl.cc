#include "ml307_ssl.h"
#include <esp_log.h>

static const char *TAG = "Ml307Ssl";

Ml307Ssl::Ml307Ssl(std::shared_ptr<AtUart> at_uart, int tcp_id) : Ml307Tcp(at_uart, tcp_id) {
}

bool Ml307Ssl::ConfigureSsl(int port) {
    char command[64];
    
    // 设置 SSL 配置
    sprintf(command, "AT+MSSLCFG=\"auth\",0,0");
    if (!at_uart_->SendCommand(command)) {
        ESP_LOGE(TAG, "Failed to set SSL configuration");
        return false;
    }

    // 强制启用 SSL
    sprintf(command, "AT+MIPCFG=\"ssl\",%d,1,0", tcp_id_);
    if (!at_uart_->SendCommand(command)) {
        ESP_LOGE(TAG, "Failed to set SSL configuration");
        return false;
    }

    return true;
} 