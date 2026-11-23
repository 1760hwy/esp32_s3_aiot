#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool is_dhcp;        // true: DHCP 动态分配, false: Static 静态 IP
    char ip[16];         // IP 地址
    char netmask[16];    // 子网掩码
    char gateway[16];    // 网关
    char dns[16];        // DNS 服务器
    char mac[18];        // MAC 地址 (只读)
    bool link_up;        // 物理链路是否连接
} eth_network_info_t;

/** 
 * @brief 在 app_main 中调用，启动 W5500 SPI 以太网驱动与网络协议栈
 */
esp_err_t app_network_init(void);

/**
 * @brief 获取当前以太网的配置与运行信息
 */
void app_network_get_eth_info(eth_network_info_t *info);

/**
 * @brief 修改以太网配置 (支持切换 DHCP / 静态 IP 并保存到 NVS，立即生效)
 */
esp_err_t app_network_set_config(bool is_dhcp, const char *ip, const char *netmask, const char *gateway, const char *dns);

/**
 * @brief 等待有线网口取得 IPv4，超时返回 ESP_ERR_TIMEOUT
 */
esp_err_t app_network_wait_for_ipv4(TickType_t timeout_ticks);

/**
 * @brief 向指定 HTTP URL 发出一次 GET (供测试有线外网出口)
 */
esp_err_t app_network_http_check(const char *url, int *status_code);

#ifdef __cplusplus
}
#endif
