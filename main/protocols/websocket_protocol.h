#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;
    
    // 摄像头推流状态管理
    void SetCameraStreaming(bool streaming);
    bool ShouldKeepConnection() const;
    bool SendKeepalive();

    void SendMcpMessage(const std::string& message) override;
    
    // 类型检查
    bool IsWebsocketProtocol() const override { return true; }

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;
    bool camera_streaming_ = false;  // 摄像头推流状态
    std::chrono::steady_clock::time_point last_keepalive_time_;  // 最后保活时间

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};

#endif
