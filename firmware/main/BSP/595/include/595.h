#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"

#define LED_595_DATA_PIN GPIO_NUM_38 //串行数据SER
#define LED_595_CLK_PIN GPIO_NUM_39 //移位时钟SRCLK
#define LED_595_LATCH_PIN GPIO_NUM_42 //锁存输出 MIMS

// 2. 指示灯在 16 位移位寄存器中的位掩码定义 (0 导通点亮, 1 截止熄灭)
// 第一片 U1 (后移入的数据 / 低 8 位):
#define LED_BIT_DI1        (1 << 0)
#define LED_BIT_DI2        (1 << 1)
#define LED_BIT_DI3        (1 << 2)
#define LED_BIT_DI4        (1 << 3)
#define LED_BIT_DO1        (1 << 4)
#define LED_BIT_DO2        (1 << 5)
#define LED_BIT_DO3        (1 << 6)
#define LED_BIT_DO4        (1 << 7)

// 第二片 U4 (先移入的数据 / 高 8 位):
#define LED_BIT_ETH        (1 << 8)
#define LED_BIT_CAN        (1 << 9)
#define LED_BIT_RS485      (1 << 10)

#define LED_595_ALL_OFF    (0xFFFF)
#define LED_595_ALL_ON     (~(0x07FF)) // 仅点亮连接的 11 颗灯 (Bit 0~10)

// 3. 对外接口声明
void led_74hc595_init(void);
void led_74hc595_write(uint16_t data16);
void led_74hc595_set_single(uint8_t bit_index);