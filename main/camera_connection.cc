#include "camera_connection.h"
#include "settings.h"
#include "system_info.h"
#include "board.h"
#include "protocols/websocket_protocol.h"
#include <esp_log.h>
#include <cJSON.h>
#include <thread>
#include <chrono>
#include <mutex>
#include <exception>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cassert>
#include <cctype>
#include <climits>
#include <cmath>
#include <cfloat>
#include <clocale>
#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdbool>
#include <cstdalign>

#define TAG "CameraConnection"

CameraConnection::CameraConnection() 
    : should_connect_(false)
    , is_connected_(false)
    , protocol_version_(3)
    , reconnect_interval_seconds_(5)
    , max_reconnect_attempts_(10)
    , current_reconnect_attempts_(0)
    , heartbeat_interval_seconds_(30) {
}

CameraConnection::~CameraConnection() {
    Stop();
}

void CameraConnection::Start() {
    ESP_LOGI(TAG, "Starting camera connection manager");
    
    // 读取连接配置
    Settings settings("websocket", false);
    server_url_ = settings.GetString("url");
    device_token_ = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        protocol_version_ = version;
    }
    
    if (server_url_.empty()) {
        ESP_LOGE(TAG, "No server URL configured for camera connection");
        return;
    }
    
    should_connect_ = true;
    current_reconnect_attempts_ = 0;
    
    // 启动连接线程
    connection_thread_ = std::thread(&CameraConnection::ConnectLoop, this);
    
    ESP_LOGI(TAG, "Camera connection manager started");
}

void CameraConnection::Stop() {
    ESP_LOGI(TAG, "Stopping camera connection manager");
    
    should_connect_ = false;
    is_connected_ = false;
    
    if (connection_thread_.joinable()) {
        connection_thread_.join();
    }
    
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    
    if (websocket_protocol_) {
        websocket_protocol_->CloseAudioChannel();
        websocket_protocol_.reset();
    }
    
    ESP_LOGI(TAG, "Camera connection manager stopped");
}

bool CameraConnection::IsConnected() const {
    return is_connected_ && websocket_protocol_ && websocket_protocol_->IsAudioChannelOpened();
}

bool CameraConnection::SendMcpMessage(const std::string& message) {
    if (!IsConnected()) {
        ESP_LOGW(TAG, "Cannot send MCP message: not connected");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(connection_mutex_);
    if (websocket_protocol_) {
        websocket_protocol_->SendMcpMessage(message);
        return true;
    }
    
    return false;
}

void CameraConnection::SetOnConnected(std::function<void()> callback) {
    on_connected_ = callback;
}

void CameraConnection::SetOnDisconnected(std::function<void()> callback) {
    on_disconnected_ = callback;
}

void CameraConnection::SetOnMessage(std::function<void(const std::string&)> callback) {
    on_message_ = callback;
}

void CameraConnection::ConnectLoop() {
    ESP_LOGI(TAG, "Camera connection loop started");
    
    while (should_connect_) {
        if (IsConnected()) {
            // 连接正常，等待一段时间后检查
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        
        // 尝试连接
        if (Connect()) {
            current_reconnect_attempts_ = 0;
            last_connection_time_ = std::chrono::steady_clock::now();
            
            if (on_connected_) {
                on_connected_();
            }
            
            // 启动心跳线程
            if (heartbeat_thread_.joinable()) {
                heartbeat_thread_.join();
            }
            heartbeat_thread_ = std::thread(&CameraConnection::SendHeartbeat, this);
            
        } else {
            // 连接失败，等待重连
            current_reconnect_attempts_++;
            if (current_reconnect_attempts_ >= max_reconnect_attempts_) {
                ESP_LOGE(TAG, "Max reconnection attempts reached, stopping camera connection");
                break;
            }
            
            ESP_LOGW(TAG, "Connection failed, retrying in %d seconds (attempt %d/%d)", 
                    reconnect_interval_seconds_, current_reconnect_attempts_, max_reconnect_attempts_);
            
            std::this_thread::sleep_for(std::chrono::seconds(reconnect_interval_seconds_));
        }
    }
    
    ESP_LOGI(TAG, "Camera connection loop ended");
}

bool CameraConnection::Connect() {
    ESP_LOGI(TAG, "Attempting to connect to camera server: %s", server_url_.c_str());
    
    try {
        // 创建WebSocket协议实例
        websocket_protocol_ = std::make_unique<WebsocketProtocol>();
        
        // 设置连接参数（如果需要的话）
        // 注意：WebsocketProtocol会从Settings中读取配置，所以我们需要确保Settings中有正确的配置
        
        // 设置消息回调
        websocket_protocol_->OnIncomingJson([this](const cJSON* root) {
            if (on_message_) {
                char* json_string = cJSON_Print(root);
                if (json_string) {
                    on_message_(std::string(json_string));
                    free(json_string);
                }
            }
        });
        
        websocket_protocol_->OnAudioChannelOpened([this]() {
            ESP_LOGI(TAG, "Camera connection established");
            is_connected_ = true;
        });
        
        websocket_protocol_->OnAudioChannelClosed([this]() {
            ESP_LOGI(TAG, "Camera connection closed");
            is_connected_ = false;
            if (on_disconnected_) {
                on_disconnected_();
            }
        });
        
        // 尝试打开音频通道（这里复用现有的连接逻辑）
        ESP_LOGI(TAG, "Opening camera audio channel...");
        if (!websocket_protocol_->OpenAudioChannel()) {
            ESP_LOGE(TAG, "Failed to open camera connection");
            websocket_protocol_.reset();
            return false;
        }
        
        ESP_LOGI(TAG, "Camera connection successful");
        return true;
        
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "Exception during camera connection: %s", e.what());
        websocket_protocol_.reset();
        return false;
    }
}

void CameraConnection::SendHeartbeat() {
    ESP_LOGI(TAG, "Camera heartbeat thread started");
    
    while (should_connect_ && IsConnected()) {
        std::this_thread::sleep_for(std::chrono::seconds(heartbeat_interval_seconds_));
        
        if (!IsConnected()) {
            break;
        }
        
        // 发送心跳消息
        cJSON* heartbeat = cJSON_CreateObject();
        cJSON_AddStringToObject(heartbeat, "type", "heartbeat");
        cJSON_AddStringToObject(heartbeat, "device_id", SystemInfo::GetMacAddress().c_str());
        cJSON_AddNumberToObject(heartbeat, "timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        
        char* json_string = cJSON_Print(heartbeat);
        if (json_string) {
            if (SendMcpMessage(std::string(json_string))) {
                last_heartbeat_time_ = std::chrono::steady_clock::now();
                ESP_LOGD(TAG, "Heartbeat sent");
            } else {
                ESP_LOGW(TAG, "Failed to send heartbeat");
            }
            free(json_string);
        }
        
        cJSON_Delete(heartbeat);
    }
    
    ESP_LOGI(TAG, "Camera heartbeat thread ended");
}
