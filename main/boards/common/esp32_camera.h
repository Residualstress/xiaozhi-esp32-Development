#ifndef ESP32_CAMERA_H
#define ESP32_CAMERA_H

#include <esp_camera.h>
#include <lvgl.h>
#include <thread>
#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "camera.h"

struct JpegChunk {
    uint8_t* data;
    size_t len;
};

class Esp32Camera : public Camera {
private:
    camera_fb_t* fb_ = nullptr;
    lv_img_dsc_t preview_image_;
    std::string explain_url_;
    std::string explain_token_;
    std::thread encoder_thread_;
    camera_config_t config_copy_{};
    bool inited_ = false;
    bool streaming_ = false;  // 推流状态
    class WebsocketProtocol* websocket_protocol_ = nullptr;  // websocket协议引用

public:
    Esp32Camera(const camera_config_t& config);
    ~Esp32Camera();

    virtual void SetExplainUrl(const std::string& url, const std::string& token);
    virtual bool Capture();
    // 翻转控制函数
    virtual bool SetHMirror(bool enabled) override;
    virtual bool SetVFlip(bool enabled) override;
    virtual std::string Explain(const std::string& question);

    // 控制硬件启停以降低发热
    bool StartCamera() override;
    void StopCamera() override;
    bool IsStarted() const override { return inited_; }
    
    // 推流控制
    void SetWebsocketProtocol(class WebsocketProtocol* protocol);
    bool StartStreaming(int fps = 8, int quality = 12);
    void StopStreaming();
    bool IsStreaming() const;
    
    // 类型检查
    bool IsEsp32Camera() const override { return true; }
};

#endif // ESP32_CAMERA_H