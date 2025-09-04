#include "esp32_camera.h"
#include "mcp_server.h"
#include "display.h"
#include "board.h"
#include "system_info.h"
#include "websocket_protocol.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <img_converters.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Esp32Camera"

Esp32Camera::Esp32Camera(const camera_config_t& config) {
    config_copy_ = config;
    // camera init
    esp_err_t err = esp_camera_init(&config_copy_); // 配置上面定义的参数
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return;
    }
    inited_ = true;

    sensor_t *s = esp_camera_sensor_get(); // 获取摄像头型号
    if (s) {
        ESP_LOGI(TAG, "Camera sensor initialized successfully");
        ESP_LOGI(TAG, "Sensor ID: PID=0x%04X, VER=0x%04X", s->id.PID, s->id.VER);
        
        if (s->id.PID == GC0308_PID) {
            ESP_LOGI(TAG, "GC0308 sensor detected, setting mirror");
            s->set_hmirror(s, 0);  // 这里控制摄像头镜像 写1镜像 写0不镜像
        } else {
            ESP_LOGW(TAG, "Unknown sensor PID: 0x%04X", s->id.PID);
        }

        // 同步传感器状态，确保分辨率/质量与当前配置匹配
        // 注意：质量参数主要影响硬件JPEG模式；RGB565模式下无明显作用，但保留以便切回JPEG时生效
        s->set_framesize(s, config_copy_.frame_size);
        s->set_quality(s, 14);
    } else {
        ESP_LOGE(TAG, "Failed to get camera sensor");
        return;
    }

    // 初始化预览图片的内存
    memset(&preview_image_, 0, sizeof(preview_image_));
    preview_image_.header.magic = LV_IMAGE_HEADER_MAGIC;
    preview_image_.header.cf = LV_COLOR_FORMAT_RGB565;
    preview_image_.header.flags = 0;

    switch (config.frame_size) {
        case FRAMESIZE_SVGA:
            preview_image_.header.w = 800;
            preview_image_.header.h = 600;
            break;
        case FRAMESIZE_VGA:
            preview_image_.header.w = 640;
            preview_image_.header.h = 480;
            break;
        case FRAMESIZE_QQVGA:
            preview_image_.header.w = 160;
            preview_image_.header.h = 120;
            break;
        case FRAMESIZE_QVGA:
            preview_image_.header.w = 320;
            preview_image_.header.h = 240;
            break;
        case FRAMESIZE_128X128:
            preview_image_.header.w = 128;
            preview_image_.header.h = 128;
            break;
        case FRAMESIZE_240X240:
            preview_image_.header.w = 240;
            preview_image_.header.h = 240;
            break;
        default:
            ESP_LOGE(TAG, "Unsupported frame size: %d, image preview will not be shown", config.frame_size);
            preview_image_.data_size = 0;
            preview_image_.data = nullptr;
            return;
    }

    preview_image_.header.stride = preview_image_.header.w * 2;
    preview_image_.data_size = preview_image_.header.w * preview_image_.header.h * 2;
    preview_image_.data = (uint8_t*)heap_caps_malloc(preview_image_.data_size, MALLOC_CAP_SPIRAM);
    if (preview_image_.data == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for preview image");
        return;
    }

    // 丢弃上电后的若干帧，避免初始化不稳定导致超时或 NO-SOI
    for (int i = 0; i < 8; ++i) {
        camera_fb_t* warm_fb = esp_camera_fb_get();
        if (warm_fb) {
            esp_camera_fb_return(warm_fb);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

Esp32Camera::~Esp32Camera() {
    StopCamera();
}

void Esp32Camera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() {
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    int frames_to_get = 2;
    // Try to get a stable frame
    for (int i = 0; i < frames_to_get; i++) {
        if (fb_ != nullptr) {
            esp_camera_fb_return(fb_);
        }
        fb_ = esp_camera_fb_get();
        if (fb_ == nullptr) {
            ESP_LOGE(TAG, "Camera capture failed");
            return false;
        }
    }

    // 如果预览图片 buffer 为空，则跳过预览
    // 但仍返回 true，因为此时图像可以上传至服务器
    if (preview_image_.data_size == 0) {
        ESP_LOGW(TAG, "Skip preview because of unsupported frame size");
        return true;
    }
    if (preview_image_.data == nullptr) {
        ESP_LOGE(TAG, "Preview image data is not initialized");
        return true;
    }
    // 若为 JPEG 模式则跳过 RGB565 预览拷贝
    if (fb_->format == PIXFORMAT_RGB565) {
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) {
            auto src = (uint16_t*)fb_->buf;
            auto dst = (uint16_t*)preview_image_.data;
            size_t pixel_count = fb_->len / 2;
            for (size_t i = 0; i < pixel_count; i++) {
                dst[i] = __builtin_bswap16(src[i]);
            }
            display->SetPreviewImage(&preview_image_);
        }
    }
    return true;
}

bool Esp32Camera::StartCamera() {
    if (inited_) return true;
    ESP_LOGI(TAG, "StartCamera with SDA=%d SCL=%d", config_copy_.pin_sccb_sda, config_copy_.pin_sccb_scl);
    esp_err_t err = esp_camera_init(&config_copy_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return false;
    }
    inited_ = true;
    return true;
}

void Esp32Camera::StopCamera() {
    if (!inited_) return;
    if (fb_) {
        esp_camera_fb_return(fb_);
        fb_ = nullptr;
    }
    if (preview_image_.data) {
        heap_caps_free((void*)preview_image_.data);
        preview_image_.data = nullptr;
    }
    esp_camera_deinit();
    inited_ = false;
}
bool Esp32Camera::SetHMirror(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (s == nullptr) {
        ESP_LOGE(TAG, "Failed to get camera sensor");
        return false;
    }
    
    esp_err_t err = s->set_hmirror(s, enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set horizontal mirror: %d", err);
        return false;
    }
    
    ESP_LOGI(TAG, "Camera horizontal mirror set to: %s", enabled ? "enabled" : "disabled");
    return true;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (s == nullptr) {
        ESP_LOGE(TAG, "Failed to get camera sensor");
        return false;
    }
    
    esp_err_t err = s->set_vflip(s, enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set vertical flip: %d", err);
        return false;
    }
    
    ESP_LOGI(TAG, "Camera vertical flip set to: %s", enabled ? "enabled" : "disabled");
    return true;
}

/**
 * @brief 将摄像头捕获的图像发送到远程服务器进行AI分析和解释
 * 
 * 该函数将当前摄像头缓冲区中的图像编码为JPEG格式，并通过HTTP POST请求
 * 以multipart/form-data的形式发送到指定的解释服务器。服务器将根据提供的
 * 问题对图像进行AI分析并返回结果。
 * 
 * 实现特点：
 * - 使用独立线程编码JPEG，与主线程分离
 * - 采用分块传输编码(chunked transfer encoding)优化内存使用
 * - 通过队列机制实现编码线程和发送线程的数据同步
 * - 支持设备ID、客户端ID和认证令牌的HTTP头部配置
 * 
 * @param question 要向AI提出的关于图像的问题，将作为表单字段发送
 * @return std::string 服务器返回的JSON格式响应字符串
 *         成功时包含AI分析结果，失败时包含错误信息
 *         格式示例：{"success": true, "result": "分析结果"}
 *                  {"success": false, "message": "错误信息"}
 * 
 * @note 调用此函数前必须先调用SetExplainUrl()设置服务器URL
 * @note 函数会等待之前的编码线程完成后再开始新的处理
 * @warning 如果摄像头缓冲区为空或网络连接失败，将返回错误信息
 */
std::string Esp32Camera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        return "{\"success\": false, \"message\": \"Image explain URL or token is not set\"}";
    }

    // 创建局部的 JPEG 队列, 40 entries is about to store 512 * 40 = 20480 bytes of JPEG data
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        return "{\"success\": false, \"message\": \"Failed to create JPEG queue\"}";
    }

    // We spawn a thread to encode the image to JPEG
    encoder_thread_ = std::thread([this, jpeg_queue]() {
        frame2jpg_cb(fb_, 50, [](void* arg, size_t index, const void* data, size_t len) -> unsigned int {
            auto jpeg_queue = (QueueHandle_t)arg;
            JpegChunk chunk = {
                .data = (uint8_t*)heap_caps_aligned_alloc(16, len, MALLOC_CAP_SPIRAM),
                .len = len
            };
            memcpy(chunk.data, data, len);
            xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
            return len;
        }, jpeg_queue);
    });

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    // 构造multipart/form-data请求体
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    // 配置HTTP客户端，使用分块传输编码
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (!http->Open("POST", explain_url_)) {
        ESP_LOGE(TAG, "Failed to connect to explain URL");
        // Clear the queue
        encoder_thread_.join();
        JpegChunk chunk;
        while (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) == pdPASS) {
            if (chunk.data != nullptr) {
                heap_caps_free(chunk.data);
            } else {
                break;
            }
        }
        vQueueDelete(jpeg_queue);
        return "{\"success\": false, \"message\": \"Failed to connect to explain URL\"}";
    }
    
    {
        // 第一块：question字段
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        http->Write(question_field.c_str(), question_field.size());
    }
    {
        // 第二块：文件字段头部
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    // 第三块：JPEG数据
    size_t total_sent = 0;
    while (true) {
        JpegChunk chunk;
        if (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk");
            break;
        }
        if (chunk.data == nullptr) {
            break; // The last chunk
        }
        http->Write((const char*)chunk.data, chunk.len);
        total_sent += chunk.len;
        heap_caps_free(chunk.data);
    }
    // Wait for the encoder thread to finish
    encoder_thread_.join();
    // 清理队列
    vQueueDelete(jpeg_queue);

    {
        // 第四块：multipart尾部
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        http->Write(multipart_footer.c_str(), multipart_footer.size());
    }
    // 结束块
    http->Write("", 0);

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", http->GetStatusCode());
        return "{\"success\": false, \"message\": \"Failed to upload photo\"}";
    }

    std::string result = http->ReadAll();
    http->Close();

    // Get remain task stack size
    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG, "Explain image size=%dx%d, compressed size=%d, remain stack size=%d, question=%s\n%s",
        fb_->width, fb_->height, total_sent, remain_stack_size, question.c_str(), result.c_str());
    return result;
}

// 设置websocket协议引用
void Esp32Camera::SetWebsocketProtocol(WebsocketProtocol* protocol) {
    websocket_protocol_ = protocol;
}

// 开始推流
bool Esp32Camera::StartStreaming(int fps, int quality) {
    if (streaming_) {
        ESP_LOGW(TAG, "推流已经在运行中");
        return true;
    }
    
    if (!inited_) {
        ESP_LOGE(TAG, "摄像头未初始化，无法开始推流");
        return false;
    }
    
    streaming_ = true;
    // 确保传感器参数与请求一致，避免取帧超时（尽量降低负载）
    if (inited_) {
        sensor_t *s = esp_camera_sensor_get();
        if (s) {
            s->set_framesize(s, FRAMESIZE_QQVGA);
            s->set_quality(s, quality);
        }
        // 丢弃更多热身帧，确保进入稳定状态
        for (int i = 0; i < 5; ++i) {
            camera_fb_t* warm = esp_camera_fb_get();
            if (warm) esp_camera_fb_return(warm);
        }
    }
    
    // 通知websocket协议保持连接
    if (websocket_protocol_) {
        websocket_protocol_->SetCameraStreaming(true);
    }
    
    ESP_LOGI(TAG, "摄像头推流开始 - FPS: %d, 质量: %d", fps, quality);
    ESP_LOGI(TAG, "注意：实际推流由MCP工具处理，这里只设置状态标志");
    return true;
}

// 停止推流
void Esp32Camera::StopStreaming() {
    if (!streaming_) {
        ESP_LOGW(TAG, "推流未在运行");
        return;
    }
    
    streaming_ = false;
    
    // 通知websocket协议恢复正常连接行为
    if (websocket_protocol_) {
        websocket_protocol_->SetCameraStreaming(false);
    }
    
    ESP_LOGI(TAG, "摄像头推流停止");
}

// 检查是否正在推流
bool Esp32Camera::IsStreaming() const {
    return streaming_;
}
