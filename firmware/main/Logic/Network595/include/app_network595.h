#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化网络与 595 指示灯联动模块
 */
void app_network595_init(void);

/**
 * @brief 设置以太网 (ETH) 指示灯状态
 * @param is_connected true: 网络已连接并获取到IP (点亮ETH灯)
 *                     false: 网络断开 (熄灭ETH灯)
 */
void app_network595_set_eth_state(bool is_connected);

/**
 * @brief 获取当前以太网连接指示状态
 * @return true 已连接, false 未连接
 */
bool app_network595_get_eth_state(void);

#ifdef __cplusplus
}
#endif