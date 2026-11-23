#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DO1_PIN   GPIO_NUM_8
#define DO2_PIN   GPIO_NUM_9
#define DO3_PIN   GPIO_NUM_40
#define DO4_PIN   GPIO_NUM_41

// 把 4 个引脚组合成一个 64 位位掩码
#define DO_PINS_MASK  ((1ULL << DO1_PIN) | (1ULL << DO2_PIN) | (1ULL << DO3_PIN) | (1ULL << DO4_PIN))

/**
 * @brief 初始化 DO1~DO4 输出通道（默认输出低电平）
 */
void bsp_do_init(void);

/**
 * @brief 设置指定通道的输出状态
 * @param channel 通道号，有效范围 1~4
 * @param enable  true 输出高电平，false 输出低电平
 */
void bsp_do_set(uint8_t channel, bool enable);

/**
 * @brief 读取指定通道当前的实际输出电平状态
 * @param channel 通道号 1~4
 * @return true 开启(高电平), false 关闭(低电平)
 */
bool bsp_do_get(uint8_t channel);

/**
 * @brief 同时设置所有 DO 通道的输出状态
 * @param enable true 全部输出高电平，false 全部输出低电平
 */
void bsp_do_set_all(bool enable);

#ifdef __cplusplus
}
#endif
