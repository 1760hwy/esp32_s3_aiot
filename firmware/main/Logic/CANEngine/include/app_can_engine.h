#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char time[12];     // "HH:MM:SS"
    char dir[4];      // "TX" or "RX"
    uint32_t id;       // CAN ID (e.g. 0x101)
    uint8_t len;       // DLC (0-8)
    bool is_ext;       // true=扩展帧, false=标准帧
    uint8_t data[8];   // payload
    char desc[32];     // 业务描述 (例如 "点亮 STM32 D0", "STM32 KEY0 触发")
} can_log_item_t;

typedef struct {
    bool is_running;
    uint32_t tx_count;
    uint32_t rx_count;
    bool stm32_d0_state;        // 当前最后下发的 D0 状态
    bool stm32_key0_triggered;  // 是否收到过 STM32 KEY0
    char last_key0_time[16];    // 最后一次收到 KEY0 的时间
    char last_rx_desc[48];      // 最后一次接收简述
} can_engine_status_t;

/**
 * @brief 初始化 CAN 业务引擎并启动后台监听任务
 */
esp_err_t app_can_engine_init(void);

/**
 * @brief 发送通用 CAN 报文并记录日志
 */
esp_err_t app_can_engine_send(uint32_t id, const uint8_t *data, uint8_t len, bool is_ext, const char *desc);

/**
 * @brief 快捷控制启明欣欣 STM32F407 板载 D0 (ID: 0x101)
 * @param on true=点亮 (01), false=熄灭 (00)
 */
esp_err_t app_can_engine_control_stm32_d0(bool on);

/**
 * @brief 获取当前 CAN 引擎状态
 */
void app_can_engine_get_status(can_engine_status_t *out_status);

/**
 * @brief 将最新的 CAN 通信报文日志序列化为 JSON 字符串
 * @param buf 存储 JSON 的缓冲区
 * @param max_len 缓冲区最大容量
 */
void app_can_engine_get_logs_json(char *buf, size_t max_len);

/**
 * @brief 清空 CAN 报文日志记录
 */
void app_can_engine_clear_logs(void);

/**
 * @brief 获取 CAN 指示灯状态 (与 595 移位寄存器联动)
 * @return true 点亮, false 熄灭
 */
bool app_can_engine_get_led_state(void);

#ifdef __cplusplus
}
#endif
