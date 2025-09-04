#include "board.h"
#include "application.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_camera.h>

static const char* TAG = "CamHttp";

static httpd_handle_t s_httpd = nullptr;
static bool s_streaming = false;

static esp_err_t stream_handler(httpd_req_t* req) {
    ESP_LOGI(TAG, "stream_handler called");
    if (!s_streaming) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Camera not started");
        return ESP_OK;
    }

    const char* boundary = "frame";
    char part_buf[64];
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "close");

    auto camera = Board::GetInstance().GetCamera();
    if (camera == nullptr) {
        httpd_resp_set_status(req, "500");
        httpd_resp_sendstr(req, "No camera");
        return ESP_OK;
    }

    int empty_count = 0;
    while (s_streaming) {
        auto fb = esp_camera_fb_get();
        if (!fb) {
            if ((++empty_count % 50) == 0) {
                ESP_LOGW(TAG, "esp_camera_fb_get returned null %d times", empty_count);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        empty_count = 0;
        const uint8_t* data = fb->buf;
        size_t len = fb->len;
        if (fb->format != PIXFORMAT_JPEG) {
            // 转JPEG
            uint8_t* jpg_buf = nullptr;
            size_t jpg_len = 0;
            if (!frame2jpg(fb, 80, &jpg_buf, &jpg_len)) {
                esp_camera_fb_return(fb);
                httpd_resp_send_500(req);
                break;
            }
            esp_camera_fb_return(fb);

            int hlen = snprintf(part_buf, sizeof(part_buf), "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", boundary, (unsigned)jpg_len);
            if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
                httpd_resp_send_chunk(req, (const char*)jpg_buf, jpg_len) != ESP_OK ||
                httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
                free(jpg_buf);
                break;
            }
            free(jpg_buf);
        } else {
            int hlen = snprintf(part_buf, sizeof(part_buf), "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", boundary, (unsigned)len);
            if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK ||
                httpd_resp_send_chunk(req, (const char*)data, len) != ESP_OK ||
                httpd_resp_send_chunk(req, "\r\n", 2) != ESP_OK) {
                esp_camera_fb_return(fb);
                break;
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_OK;
}

static esp_err_t start_handler(httpd_req_t* req) {
    ESP_LOGI(TAG, "start_handler called");
    auto cam = Board::GetInstance().GetCamera();
    if (cam && !cam->IsStarted()) {
        if (!cam->StartCamera()) {
            httpd_resp_set_status(req, "500");
            httpd_resp_sendstr(req, "Start camera failed");
            return ESP_OK;
        }
    }
    s_streaming = true;
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static esp_err_t stop_handler(httpd_req_t* req) {
    ESP_LOGI(TAG, "stop_handler called");
    s_streaming = false;
    auto cam = Board::GetInstance().GetCamera();
    if (cam && cam->IsStarted()) {
        cam->StopCamera();
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

static void register_routes(httpd_handle_t server) {
    // Simple index page with control buttons
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req){
            static const char html[] =
                "<html><head><meta name=viewport content=width=device-width,initial-scale=1>"
                "<style>body{font-family:sans-serif;margin:16px}button{padding:8px 16px;margin-right:8px}img{max-width:100%;height:auto;border:1px solid #ccc}</style>"
                "</head><body>"
                "<h3>Camera Control</h3>"
                "<button onclick=(()=>{fetch('/camera/start',{method:'POST'}).then(()=>{const i=document.getElementById('img');i.src='/stream?ts='+Date.now();});})()>Start</button>"
                "<button onclick=fetch('/camera/stop',{method:'POST'}).then(()=>{const i=document.getElementById('img');i.src='about:blank';})>Stop</button>"
                "<script>function retry(){const i=document.getElementById('img');if(!i.src||i.src==='about:blank'){i.src='/stream?ts='+Date.now();}}setInterval(retry,3000);</script>"
                "<div style='margin-top:12px'><img id=img src='' onerror=console.log('wait start')></div>"
                "</body></html>";
            httpd_resp_set_type(req, "text/html");
            httpd_resp_send(req, html, sizeof(html)-1);
            return ESP_OK;
        },
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &stream_uri);

    // Single snapshot endpoint
    httpd_uri_t snapshot_uri = {
        .uri = "/snapshot",
        .method = HTTP_GET,
        .handler = [](httpd_req_t* req){
            ESP_LOGI(TAG, "snapshot handler called");
            auto camera = Board::GetInstance().GetCamera();
            if (camera == nullptr) {
                httpd_resp_set_status(req, "500");
                httpd_resp_sendstr(req, "No camera");
                return ESP_OK;
            }
            camera_fb_t* fb = esp_camera_fb_get();
            if (!fb) {
                httpd_resp_set_status(req, "503 Service Unavailable");
                httpd_resp_sendstr(req, "Frame not ready");
                return ESP_OK;
            }
            uint8_t* out_buf = nullptr;
            size_t out_len = 0;
            if (fb->format == PIXFORMAT_JPEG) {
                httpd_resp_set_type(req, "image/jpeg");
                httpd_resp_send(req, (const char*)fb->buf, fb->len);
                esp_camera_fb_return(fb);
                return ESP_OK;
            }
            bool ok = frame2jpg(fb, 80, &out_buf, &out_len);
            esp_camera_fb_return(fb);
            if (!ok) {
                httpd_resp_send_500(req);
                return ESP_OK;
            }
            httpd_resp_set_type(req, "image/jpeg");
            httpd_resp_send(req, (const char*)out_buf, out_len);
            free(out_buf);
            return ESP_OK;
        },
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &snapshot_uri);

    httpd_uri_t start_uri = {
        .uri = "/camera/start",
        .method = HTTP_POST,
        .handler = start_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &start_uri);

    httpd_uri_t stop_uri = {
        .uri = "/camera/stop",
        .method = HTTP_POST,
        .handler = stop_handler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &stop_uri);
}

extern "C" void cam_http_server_start() {
    if (s_httpd) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    if (httpd_start(&s_httpd, &config) == ESP_OK) {
        register_routes(s_httpd);
        ESP_LOGI(TAG, "HTTP server started on :%d", config.server_port);
    } else {
        ESP_LOGE(TAG, "Failed to start HTTP server");
    }
}

extern "C" void cam_http_server_stop() {
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = nullptr;
}


