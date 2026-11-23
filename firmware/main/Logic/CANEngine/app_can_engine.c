/**
 * @file app_can_engine.c
 * @brief CAN 工业通信与 STM32 联动业务引擎实现
 */

#include "app_can_engine.h"
#include "app_can.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "CAN_ENGINE";

#define MAX_CAN_LOGS 15
static can_log_item_t s_logs[MAX_CAN_LOGS];
static int s_log_head = 0;
static int s_log_count = 0;
static SemaphoreHandle_t s_lock = NULL;

static can_engine_status_t s_status = {
    .is_running = false,
    .tx_count = 0,
    .rx_count = 0,
    .stm32_d0_state = false,
    .stm32_key0_triggered = false,
    .last_key0_time = "无",
    .last_rx_desc = "等待从机报文...",
};

static bool s_can_led_active = false;
static uint32_t s_can_led_off_time = 0;

// 触发 CAN 595 指示灯闪烁 (200ms，在 595 50ms 刷新周期中清晰饱满)
static void trigger_can_led(void)
{
    s_can_led_active = true;
    s_can_led_off_time = (uint32_t)(esp_timer_get_time() / 1000) + 200;
}

bool app_can_engine_get_led_state(void)
{
    if (s_can_led_active) {
        if ((uint32_t)(esp_timer_get_time() / 1000) >= s_can_led_off_time) {
            s_can_led_active = false;
        }
    }
    return s_can_led_active;
}

static void get_uptime_str(char *buf, size_t max_len)
{
    uint32_t total_sec = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t h = (total_sec / 3600) % 24;
    uint32_t m = (total_sec / 60) % 60;
    uint32_t s = total_sec % 60;
    snprintf(buf, max_len, "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)s);
}

static void add_log(const char *dir, uint32_t id, const uint8_t *data, uint8_t len, bool is_ext, const char *desc)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    can_log_item_t *item = &s_logs[s_log_head];
    get_uptime_str(item->time, sizeof(item->time));
    strncpy(item->dir, dir, sizeof(item->dir) - 1);
    item->id = id;
    item->len = (len > 8) ? 8 : len;
    item->is_ext = is_ext;
    memset(item->data, 0, sizeof(item->data));
    if (data && item->len > 0) {
        memcpy(item->data, data, item->len);
    }
    strncpy(item->desc, desc ? desc : "", sizeof(item->desc) - 1);

    s_log_head = (s_log_head + 1) % MAX_CAN_LOGS;
    if (s_log_count < MAX_CAN_LOGS) {
        s_log_count++;
    }

    xSemaphoreGive(s_lock);
}

static void can_rx_task(void *pvParameters)
{
    ESP_LOGI(TAG, "CAN 接收后台监听任务已启动 (波特率 500kbps)");
    app_can_msg_t rx_msg;

    while (1) {
        if (app_can_receive(&rx_msg, 200)) {
            trigger_can_led();
            char desc[32] = {0};
            // 识别启明欣欣 STM32F407 报文
            if (rx_msg.id == 0x102) {
                if (rx_msg.len >= 1 && rx_msg.data[0] == 0x01) {
                    snprintf(desc, sizeof(desc), "STM32 KEY0 触发");
                } else {
                    snprintf(desc, sizeof(desc), "STM32 按键释放/状态");
                }
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_status.stm32_key0_triggered = true;
                get_uptime_str(s_status.last_key0_time, sizeof(s_status.last_key0_time));
                xSemaphoreGive(s_lock);
            } else {
                snprintf(desc, sizeof(desc), "从机数据帧");
            }

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_status.rx_count++;
            snprintf(s_status.last_rx_desc, sizeof(s_status.last_rx_desc),
                     "ID:0x%lX [%s]", (unsigned long)rx_msg.id, desc);
            xSemaphoreGive(s_lock);

            add_log("RX", rx_msg.id, rx_msg.data, rx_msg.len, rx_msg.is_ext, desc);
            ESP_LOGI(TAG, "CAN RX << ID:0x%lX Len:%d Data:%02X (%s)",
                     (unsigned long)rx_msg.id, rx_msg.len, rx_msg.len > 0 ? rx_msg.data[0] : 0, desc);
        }
    }
}

esp_err_t app_can_engine_init(void)
{
    if (s_status.is_running) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = app_can_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "初始化底层 app_can 驱动失败: %s", esp_err_to_name(err));
        return err;
    }

    BaseType_t ret = xTaskCreate(can_rx_task, "can_rx_task", 3072, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "创建 can_rx_task 任务失败");
        return ESP_FAIL;
    }

    s_status.is_running = true;
    ESP_LOGI(TAG, "CAN 工业通信引擎初始化成功！");
    return ESP_OK;
}

esp_err_t app_can_engine_send(uint32_t id, const uint8_t *data, uint8_t len, bool is_ext, const char *desc)
{
    if (!s_status.is_running) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = app_can_send(id, data, len, is_ext);
    if (err == ESP_OK) {
        trigger_can_led();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.tx_count++;
        xSemaphoreGive(s_lock);
        add_log("TX", id, data, len, is_ext, desc ? desc : "用户指令");
        ESP_LOGI(TAG, "CAN TX >> ID:0x%lX Len:%d (%s) 成功", (unsigned long)id, len, desc ? desc : "");
    } else {
        ESP_LOGW(TAG, "CAN TX >> ID:0x%lX 发送失败: %s", (unsigned long)id, esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_can_engine_control_stm32_d0(bool on)
{
    uint8_t d = on ? 0x01 : 0x00;
    esp_err_t err = app_can_engine_send(0x101, &d, 1, false, on ? "点亮 STM32 D0" : "熄灭 STM32 D0");
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.stm32_d0_state = on;
        xSemaphoreGive(s_lock);
    }
    return err;
}

void app_can_engine_get_status(can_engine_status_t *out_status)
{
    if (!out_status || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out_status, &s_status, sizeof(can_engine_status_t));
    xSemaphoreGive(s_lock);
}

void app_can_engine_get_logs_json(char *buf, size_t max_len)
{
    if (!buf || max_len == 0) return;
    buf[0] = '\0';
    if (!s_lock) {
        snprintf(buf, max_len, "[]");
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t offset = 0;
    offset += snprintf(buf + offset, max_len - offset, "[");

    for (int i = 0; i < s_log_count; i++) {
        // 从最新的日志往前读
        int idx = (s_log_head - 1 - i + MAX_CAN_LOGS) % MAX_CAN_LOGS;
        can_log_item_t *item = &s_logs[idx];

        char hex_str[32] = {0};
        for (int k = 0; k < item->len; k++) {
            char b[4];
            snprintf(b, sizeof(b), "%02X ", item->data[k]);
            strcat(hex_str, b);
        }
        if (strlen(hex_str) > 0 && hex_str[strlen(hex_str)-1] == ' ') {
            hex_str[strlen(hex_str)-1] = '\0';
        }

        int written = snprintf(buf + offset, max_len - offset,
            "%s{\"time\":\"%s\",\"dir\":\"%s\",\"id\":\"0x%lX\",\"len\":%d,\"is_ext\":%s,\"data\":\"%s\",\"desc\":\"%s\"}",
            (i > 0) ? "," : "",
            item->time,
            item->dir,
            (unsigned long)item->id,
            item->len,
            item->is_ext ? "true" : "false",
            hex_str,
            item->desc
        );
        if (written > 0 && (offset + written < max_len)) {
            offset += written;
        } else {
            break;
        }
    }

    snprintf(buf + offset, max_len - offset, "]");
    xSemaphoreGive(s_lock);
}

void app_can_engine_clear_logs(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_log_head = 0;
    s_log_count = 0;
    xSemaphoreGive(s_lock);
}
