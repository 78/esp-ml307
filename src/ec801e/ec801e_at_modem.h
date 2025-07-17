#ifndef _EC801E_AT_MODEM_H_
#define _EC801E_AT_MODEM_H_

#include "at_modem.h"

class Ec801EAtModem : public AtModem {
public:
    Ec801EAtModem(std::shared_ptr<AtUart> at_uart);
    ~Ec801EAtModem() override = default;

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


#endif // _EC801E_AT_MODEM_H_