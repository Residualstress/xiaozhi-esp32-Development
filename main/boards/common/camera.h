#ifndef CAMERA_H
#define CAMERA_H

#include <string>

class Camera {
public:
    virtual void SetExplainUrl(const std::string& url, const std::string& token) = 0;
    virtual bool Capture() = 0;
    virtual bool SetHMirror(bool enabled) = 0;
    virtual bool SetVFlip(bool enabled) = 0;
    virtual std::string Explain(const std::string& question) = 0;
    // 摄像头启停控制（默认空实现），用于功耗与发热管理
    virtual bool StartCamera() { return true; }
    virtual void StopCamera() {}
    virtual bool IsStarted() const { return true; }
    
    // 类型检查方法
    virtual bool IsEsp32Camera() const { return false; }
};

#endif // CAMERA_H
