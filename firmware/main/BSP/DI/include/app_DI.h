#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DI1_PIN   GPIO_NUM_4
#define DI2_PIN   GPIO_NUM_5
#define DI3_PIN   GPIO_NUM_6
#define DI4_PIN   GPIO_NUM_7

#define DI_PINS_MASK  ((1ULL << DI1_PIN) | (1ULL << DI2_PIN) | (1ULL << DI3_PIN) | (1ULL << DI4_PIN))

/**
 * @brief 初始化 DI1~DI4 输入通道（内部上拉，禁用中断）
 */
void bsp_di_init(void);

/**
 * @brief 读取指定通道的输入状态（已做光耦反相处理）
 * @param channel 通道号，有效范围 1~4
 * @return true 有信号，false 无信号或通道号非法
 */
bool bsp_di_get_raw(uint8_t channel);

#ifdef __cplusplus
}
#endif
