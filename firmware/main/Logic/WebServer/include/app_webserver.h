#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动独立的 WebServer 模块 (监听端口 8080)
 */
esp_err_t app_webserver_start(uint16_t port);

/**
 * @brief 停止 WebServer 模块
 */
void app_webserver_stop(void);

#ifdef __cplusplus
}
#endif
