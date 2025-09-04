#pragma once

#include <memory>
#include <string>
#include <functional>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include "websocket_protocol.h"

class CameraConnection {
public:
    CameraConnection();
    ~CameraConnection();

    // 启动摄像头连接（在设备初始化后调用）
    void Start();
    
    // 停止摄像头连接
    void Stop();
    
    // 检查连接状态
    bool IsConnected() const;
    
    // 发送MCP消息
    bool SendMcpMessage(const std::string& message);
    
    // 设置回调函数
    void SetOnConnected(std::function<void()> callback);
    void SetOnDisconnected(std::function<void()> callback);
    void SetOnMessage(std::function<void(const std::string&)> callback);

private:
    void ConnectLoop();
    void SendHeartbeat();
    void Reconnect();
    bool Connect();
    
    std::unique_ptr<WebsocketProtocol> websocket_protocol_;
    std::atomic<bool> should_connect_;
    std::atomic<bool> is_connected_;
    std::thread connection_thread_;
    std::thread heartbeat_thread_;
    std::mutex connection_mutex_;
    
    // 连接参数
    std::string server_url_;
    std::string device_token_;
    int protocol_version_;
    
    // 重连参数
    int reconnect_interval_seconds_;
    int max_reconnect_attempts_;
    int current_reconnect_attempts_;
    
    // 心跳参数
    int heartbeat_interval_seconds_;
    
    // 回调函数
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;
    std::function<void(const std::string&)> on_message_;
    
    // 最后连接时间
    std::chrono::steady_clock::time_point last_connection_time_;
    std::chrono::steady_clock::time_point last_heartbeat_time_;
};
