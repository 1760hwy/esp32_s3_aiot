#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
} wifi_scan_ap_info_t;

/**
 * @brief 初始化 Wi-Fi (STA 模式)
 */
esp_err_t app_wifi_init(void);

/**
 * @brief 获取当前 Wi-Fi 是否已连接并获得 IP
 */
bool app_wifi_is_connected(void);

/**
 * @brief 获取当前 Wi-Fi 的 IP 信息 (如果未连接则返回 false)
 */
bool app_wifi_get_ip_info(char *ip_buf, size_t ip_len, char *ssid_buf, size_t ssid_len);

/**
 * @brief 动态连接指定的 Wi-Fi 热点
 */
esp_err_t app_wifi_connect_to(const char *ssid, const char *password);

/**
 * @brief 扫描周围的 Wi-Fi 热点
 * @param ap_list 用于存放扫描结果的数组
 * @param max_aps 数组最大容量
 * @return 实际扫描到的 AP 数量
 */
uint16_t app_wifi_scan(wifi_scan_ap_info_t *ap_list, uint16_t max_aps);

#ifdef __cplusplus
}
#endif
