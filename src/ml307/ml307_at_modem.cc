#include "ml307_at_modem.h"
#include <esp_log.h>
#include <esp_err.h>
#include <sstream>
#include <iomanip>
#include <cstring>
#include "ml307_tcp.h"
#include "ml307_ssl.h"
#include "ml307_udp.h"
#include "ml307_mqtt.h"
#include "http_client.h"
#include "web_socket.h"

#define TAG "Ml307AtModem"


Ml307AtModem::Ml307AtModem(std::shared_ptr<AtUart> at_uart) : AtModem(at_uart) {
    // 子类特定的初始化在这里
}

void Ml307AtModem::HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments) {
    if (command == "MIPCALL" && arguments.size() >= 3) {
        if (arguments[1].int_value == 1) {
            network_ready_ = true;
            xEventGroupSetBits(event_group_handle_, AT_EVENT_NETWORK_READY);
        }
    } else if (command == "MATREADY") {
        if (network_ready_) {
            network_ready_ = false;
            if (on_network_state_changed_) {
                on_network_state_changed_(false);
            }
        }
    }
    AtModem::HandleUrc(command, arguments);
}

void Ml307AtModem::Reboot() {
    at_uart_->SendCommand("AT+MREBOOT=0");
}

void Ml307AtModem::ResetConnections() {
    // Reset HTTP instances
    at_uart_->SendCommand("AT+MHTTPDEL=0");
    at_uart_->SendCommand("AT+MHTTPDEL=1");
    at_uart_->SendCommand("AT+MHTTPDEL=2");
    at_uart_->SendCommand("AT+MHTTPDEL=3");
}

bool Ml307AtModem::SetSleepMode(bool enable, int delay_seconds) {
    if (enable) {
        if (delay_seconds > 0) {
            at_uart_->SendCommand("AT+MLPMCFG=\"delaysleep\"," + std::to_string(delay_seconds));
        }
        return at_uart_->SendCommand("AT+MLPMCFG=\"sleepmode\",2,0");
    } else {
        return at_uart_->SendCommand("AT+MLPMCFG=\"sleepmode\",0,0");
    }
}

Http* Ml307AtModem::CreateHttp(int connect_id) {
    return new HttpClient(this, connect_id);
}

Tcp* Ml307AtModem::CreateTcp(int connect_id) {
    return new Ml307Tcp(at_uart_, connect_id);
}

Tcp* Ml307AtModem::CreateSsl(int connect_id) {
    return new Ml307Ssl(at_uart_, connect_id);
}

Udp* Ml307AtModem::CreateUdp(int connect_id) {
    return new Ml307Udp(at_uart_, connect_id);
}

Mqtt* Ml307AtModem::CreateMqtt(int connect_id) {
    return new Ml307Mqtt(at_uart_, connect_id);
}

WebSocket* Ml307AtModem::CreateWebSocket(int connect_id) {
    return new WebSocket(this, connect_id);
}
