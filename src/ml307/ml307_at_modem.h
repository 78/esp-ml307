#ifndef _ML307_AT_MODEM_H_
#define _ML307_AT_MODEM_H_

#include "at_modem.h"
#include "tcp.h"
#include "udp.h"
#include "http.h"
#include "mqtt.h"
#include "web_socket.h"

class Ml307AtModem : public AtModem {
public:
    Ml307AtModem(std::shared_ptr<AtUart> at_uart);
    ~Ml307AtModem() override = default;

    void Reboot() override;
    void ResetConnections() override;
    bool SetSleepMode(bool enable, int delay_seconds=0) override;

    // 实现基类的纯虚函数
    Http* CreateHttp(int connect_id) override;
    Tcp* CreateTcp(int connect_id) override;
    Tcp* CreateSsl(int connect_id) override;
    Udp* CreateUdp(int connect_id) override;
    Mqtt* CreateMqtt(int connect_id) override;
    WebSocket* CreateWebSocket(int connect_id) override;

protected:
    void HandleUrc(const std::string& command, const std::vector<AtArgumentValue>& arguments) override;
};


#endif // _ML307_AT_MODEM_H_