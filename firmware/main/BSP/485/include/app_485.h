#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 硬件引脚定义 (与下层板原理图严格对应)
#define RS485_UART_PORT      UART_NUM_2
#define RS485_PIN_TX         GPIO_NUM_21
#define RS485_PIN_RX         GPIO_NUM_47
#define RS485_PIN_DIR        GPIO_NUM_48 // 方向控制脚 DE/RE

/**
 * @brief 初始化 RS-485 硬件串口 (配置为硬件半双工自动流控)
 * @param baud_rate 波特率 (例如 9600, 115200)
 */
esp_err_t bsp_rs485_init(uint32_t baud_rate);

/**
 * @brief 动态修改 RS-485 串口波特率
 */
esp_err_t bsp_rs485_set_baudrate(uint32_t baud_rate);

/**
 * @brief 通过 RS-485 发送一包数据 (底层硬件自动控制 DIR 切换)
 * @param data 待发送数据指针
 * @param len 数据长度
 * @return 实际发送的字节数
 */
int bsp_rs485_send(const uint8_t *data, size_t len);

/**
 * @brief 从 RS-485 接收数据 (带超时)
 * @param buf 接收缓冲区
 * @param max_len 缓冲区最大长度
 * @param timeout_ms 超时毫秒数
 * @return 实际接收到的字节数
 */
int bsp_rs485_recv(uint8_t *buf, size_t max_len, uint32_t timeout_ms);

/**
 * @brief 工业标准 Modbus CRC16 计算工具函数
 * @param data 待计算数据指针
 * @param length 数据字节长度
 * @return 16位 CRC 校验码 (低字节在前，高字节在后)
 */
uint16_t bsp_rs485_calc_crc16(const uint8_t *data, size_t length);

#ifdef __cplusplus
}
#endif
