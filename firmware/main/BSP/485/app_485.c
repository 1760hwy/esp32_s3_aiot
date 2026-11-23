/**
 * @file app_rs485.c
 * @brief RS-485 硬件底层驱动实现。
 * 
 * 核心技术点：
 *   1. 使用 ESP32 原生 UART_MODE_RS485_HALF_DUPLEX（硬件半双工）。
 *   2. DIR（GPIO 48）连接至 485 收发器的 DE/RE 控制脚。
 *   3. 发送数据时硬件自动拉高 DIR 驱动发送，发送完成后自动拉低恢复接收监听，
 *      无需软件延时，从根本上杜绝收发翻转冲突！
 */

#include "app_485.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "BSP_RS485";
static bool s_initialized = false;

esp_err_t bsp_rs485_init(uint32_t baud_rate)
{
    if (s_initialized) {
        return bsp_rs485_set_baudrate(baud_rate);
    }

    ESP_LOGI(TAG, "初始化 RS-485: TX=%d, RX=%d, DIR=%d, 波特率=%ld",
             RS485_PIN_TX, RS485_PIN_RX, RS485_PIN_DIR, baud_rate);

    // 1. 基础串口参数配置: 8N1 (8位数据位, 无校验, 1位停止位)
    const uart_config_t uart_config = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // 2. 安装串口驱动，分配接收与发送环形缓冲区 (RX 512B, TX 512B)
    ESP_ERROR_CHECK(uart_param_config(RS485_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(RS485_UART_PORT, RS485_PIN_TX, RS485_PIN_RX, RS485_PIN_DIR, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(RS485_UART_PORT, 512, 512, 0, NULL, 0));

    // 3. 核心：开启 ESP32 硬件半双工自动流控模式！
    ESP_ERROR_CHECK(uart_set_mode(RS485_UART_PORT, UART_MODE_RS485_HALF_DUPLEX));

    s_initialized = true;
    ESP_LOGI(TAG, "RS-485 硬件半双工驱动初始化成功");
    return ESP_OK;
}

esp_err_t bsp_rs485_set_baudrate(uint32_t baud_rate)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "动态调整 RS-485 波特率: %ld", baud_rate);
    return uart_set_baudrate(RS485_UART_PORT, baud_rate);
}

int bsp_rs485_send(const uint8_t *data, size_t len)
{
    if (!s_initialized || !data || len == 0) return 0;
    return uart_write_bytes(RS485_UART_PORT, (const char *)data, len);
}

int bsp_rs485_recv(uint8_t *buf, size_t max_len, uint32_t timeout_ms)
{
    if (!s_initialized || !buf || max_len == 0) return 0;
    return uart_read_bytes(RS485_UART_PORT, buf, max_len, pdMS_TO_TICKS(timeout_ms));
}

// 工业标准 Modbus CRC16 计算函数 (多项式 0xA001)
uint16_t bsp_rs485_calc_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return crc;
}
