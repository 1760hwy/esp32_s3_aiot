/**
 * @file app_rs485_engine.c
 * @brief 真正的万能 RS-485 工业设备智能控制与物模型采集引擎。
 */

#include "app_rs485_engine.h"
#include "app_485.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_timer.h"

static const char *TAG = "RS485_ENGINE";

// 总线互斥锁
static SemaphoreHandle_t s_bus_mutex = NULL;

// 点位表
static rs485_point_t s_points[MAX_RS485_POINTS];

// 后台轮询总开关 (默认开启，可在网页自由关闭/开启)
static bool s_auto_poll_enabled = true;

// 595 指示灯管理 (时间展宽至 160ms，彻底消除漏闪、暗闪)
static bool s_led_active = false;
static uint32_t s_led_off_time = 0;

#define NVS_RS485_NS "rs485_univ"

// 触发 595 指示灯饱满闪烁 (160ms，覆盖 595 50ms 刷新周期的 3 次以上)
static void trigger_485_led(void)
{
    s_led_active = true;
    s_led_off_time = (uint32_t)(esp_timer_get_time() / 1000) + 160;
}

bool app_rs485_engine_get_led_state(void)
{
    if (s_led_active) {
        if ((uint32_t)(esp_timer_get_time() / 1000) >= s_led_off_time) {
            s_led_active = false;
        }
    }
    return s_led_active;
}

static void flush_rx_fifo(void)
{
    uint8_t dummy[64];
    while (bsp_rs485_recv(dummy, sizeof(dummy), 0) > 0);
}

void app_rs485_engine_set_auto_poll(bool enable)
{
    s_auto_poll_enabled = enable;
    ESP_LOGI(TAG, "后台自动轮询采集已%s", enable ? "开启" : "暂停 (总线静止)");
}

bool app_rs485_engine_is_auto_poll(void)
{
    return s_auto_poll_enabled;
}

/* ====================================================================
 * NVS 持久化
 * ==================================================================== */

static void load_points_from_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_RS485_NS, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_points);
        if (nvs_get_blob(nvs, "table", s_points, &len) == ESP_OK) {
            ESP_LOGI(TAG, "从 NVS 恢复通用点位表成功");
            nvs_close(nvs);
            return;
        }
        nvs_close(nvs);
    }
    // 首次启动若无配置，默认生成 DAM0404D 模板 (从机 254)
    app_rs485_engine_apply_template(TEMPLATE_DAM0404D, 254);
}

static void save_points_to_nvs(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_RS485_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_blob(nvs, "table", s_points, sizeof(s_points));
        nvs_commit(nvs);
        nvs_close(nvs);
        ESP_LOGI(TAG, "点位表已持久化存盘");
    }
}

/* ====================================================================
 * 一键应用预设设备模板
 * ==================================================================== */

esp_err_t app_rs485_engine_clear_all_points(void)
{
    memset(s_points, 0, sizeof(s_points));
    save_points_to_nvs();
    return ESP_OK;
}

esp_err_t app_rs485_engine_apply_template(rs485_template_type_t template_type, uint8_t slave_addr)
{
    memset(s_points, 0, sizeof(s_points));

    if (template_type == TEMPLATE_DAM0404D) {
        // 聚英 DAM0404D 模板: 4路继电器控制(05码) + 4路光耦监测(02码)
        for (int i = 0; i < 4; i++) {
            s_points[i].enabled = true;
            snprintf(s_points[i].name, sizeof(s_points[i].name), "继电器 %d (JD%d)", i + 1, i + 1);
            s_points[i].kind = POINT_KIND_COIL_SWITCH;
            s_points[i].slave_id = slave_addr;
            s_points[i].func_code = 0x05;
            s_points[i].reg_addr = i;
            s_points[i].current_state = false;
        }
        for (int i = 0; i < 4; i++) {
            s_points[4 + i].enabled = true;
            snprintf(s_points[4 + i].name, sizeof(s_points[4 + i].name), "光耦输入 %d (IN%d)", i + 1, i + 1);
            s_points[4 + i].kind = POINT_KIND_DISCRETE_INPUT;
            s_points[4 + i].slave_id = slave_addr;
            s_points[4 + i].func_code = 0x02;
            s_points[4 + i].reg_addr = i;
            s_points[4 + i].current_state = false;
        }
    } else if (template_type == TEMPLATE_RELAY_8CH) {
        // 通用 8 路继电器板
        for (int i = 0; i < 8; i++) {
            s_points[i].enabled = true;
            snprintf(s_points[i].name, sizeof(s_points[i].name), "8路继电器 CH%d", i + 1);
            s_points[i].kind = POINT_KIND_COIL_SWITCH;
            s_points[i].slave_id = slave_addr;
            s_points[i].func_code = 0x05;
            s_points[i].reg_addr = i;
            s_points[i].current_state = false;
        }
    } else if (template_type == TEMPLATE_SHT30_ENV) {
        // 温湿度传感器
        s_points[0].enabled = true;
        strlcpy(s_points[0].name, "环境温度", sizeof(s_points[0].name));
        strlcpy(s_points[0].unit, "℃", sizeof(s_points[0].unit));
        s_points[0].kind = POINT_KIND_ANALOG_INPUT;
        s_points[0].slave_id = slave_addr;
        s_points[0].func_code = 0x03;
        s_points[0].reg_addr = 0x0000;
        s_points[0].data_type = POINT_TYPE_INT16;
        s_points[0].scale = 0.1f;

        s_points[1].enabled = true;
        strlcpy(s_points[1].name, "环境湿度", sizeof(s_points[1].name));
        strlcpy(s_points[1].unit, "%RH", sizeof(s_points[1].unit));
        s_points[1].kind = POINT_KIND_ANALOG_INPUT;
        s_points[1].slave_id = slave_addr;
        s_points[1].func_code = 0x03;
        s_points[1].reg_addr = 0x0001;
        s_points[1].data_type = POINT_TYPE_UINT16;
        s_points[1].scale = 0.1f;
    } else if (template_type == TEMPLATE_VFD_BASIC) {
        // 变频器启停控制与调速
        s_points[0].enabled = true;
        strlcpy(s_points[0].name, "变频器启停控制", sizeof(s_points[0].name));
        s_points[0].kind = POINT_KIND_COIL_SWITCH;
        s_points[0].slave_id = slave_addr;
        s_points[0].func_code = 0x05;
        s_points[0].reg_addr = 0x0000;

        s_points[1].enabled = true;
        strlcpy(s_points[1].name, "运行目标频率", sizeof(s_points[1].name));
        strlcpy(s_points[1].unit, "Hz", sizeof(s_points[1].unit));
        s_points[1].kind = POINT_KIND_HOLDING_WRITE;
        s_points[1].slave_id = slave_addr;
        s_points[1].func_code = 0x06;
        s_points[1].reg_addr = 0x0001;
        s_points[1].scale = 0.01f;
        s_points[1].current_val = 50.0f;
    }

    save_points_to_nvs();
    ESP_LOGI(TAG, "已应用模板类型 %d，从机地址 %d", template_type, slave_addr);
    return ESP_OK;
}

/* ====================================================================
 * 前台即时控制执行接口
 * ==================================================================== */

esp_err_t app_rs485_engine_control_switch(uint8_t index, bool state)
{
    if (index >= MAX_RS485_POINTS || !s_points[index].enabled) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_bus_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(1200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    rs485_point_t *pt = &s_points[index];
    uint8_t req[8];
    req[0] = pt->slave_id;
    req[1] = 0x05;                      // 功能码 05: 写单线圈
    req[2] = (pt->reg_addr >> 8) & 0xFF;
    req[3] = pt->reg_addr & 0xFF;
    req[4] = state ? 0xFF : 0x00;       // FF 00 为开，00 00 为关
    req[5] = 0x00;
    uint16_t crc = bsp_rs485_calc_crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    flush_rx_fifo();
    bsp_rs485_send(req, 8);

    uint8_t resp[16];
    int recv_len = bsp_rs485_recv(resp, 8, 300);
    esp_err_t ret = ESP_FAIL;

    if (recv_len >= 8 && resp[0] == pt->slave_id && resp[1] == 0x05) {
        uint16_t resp_crc = bsp_rs485_calc_crc16(resp, 6);
        uint16_t expected_crc = resp[6] | (resp[7] << 8);
        if (resp_crc == expected_crc) {
            trigger_485_led();
            pt->is_online = true;
            pt->current_state = state;
            pt->last_sync_time = (uint32_t)(esp_timer_get_time() / 1000000);
            ret = ESP_OK;
            ESP_LOGI(TAG, "开关点位[%d] %s 控制成功 -> %s", index, pt->name, state ? "开启" : "关闭");
        }
    }

    xSemaphoreGive(s_bus_mutex);
    return ret;
}

esp_err_t app_rs485_engine_write_value(uint8_t index, float val)
{
    if (index >= MAX_RS485_POINTS || !s_points[index].enabled) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_bus_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(1200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    rs485_point_t *pt = &s_points[index];
    uint16_t raw_val = (pt->scale > 0) ? (uint16_t)(val / pt->scale) : (uint16_t)val;

    uint8_t req[8];
    req[0] = pt->slave_id;
    req[1] = 0x06;                      // 功能码 06: 写单寄存器
    req[2] = (pt->reg_addr >> 8) & 0xFF;
    req[3] = pt->reg_addr & 0xFF;
    req[4] = (raw_val >> 8) & 0xFF;
    req[5] = raw_val & 0xFF;
    uint16_t crc = bsp_rs485_calc_crc16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    flush_rx_fifo();
    bsp_rs485_send(req, 8);

    uint8_t resp[16];
    int recv_len = bsp_rs485_recv(resp, 8, 300);
    esp_err_t ret = ESP_FAIL;

    if (recv_len >= 8 && resp[0] == pt->slave_id && resp[1] == 0x06) {
        uint16_t resp_crc = bsp_rs485_calc_crc16(resp, 6);
        uint16_t expected_crc = resp[6] | (resp[7] << 8);
        if (resp_crc == expected_crc) {
            trigger_485_led();
            pt->is_online = true;
            pt->current_val = val;
            pt->last_sync_time = (uint32_t)(esp_timer_get_time() / 1000000);
            ret = ESP_OK;
            ESP_LOGI(TAG, "设定点位[%d] %s 写入成功 -> %.2f", index, pt->name, val);
        }
    }

    xSemaphoreGive(s_bus_mutex);
    return ret;
}

esp_err_t app_rs485_engine_control_all_switches(bool state)
{
    // 遍历所有启用的开关控制型点位
    for (int i = 0; i < MAX_RS485_POINTS; i++) {
        if (s_points[i].enabled && s_points[i].kind == POINT_KIND_COIL_SWITCH) {
            app_rs485_engine_control_switch(i, state);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    return ESP_OK;
}

/* ====================================================================
 * 万能 Modbus 调试 / 自定义收发实现
 * ==================================================================== */

esp_err_t app_rs485_engine_execute_raw(const modbus_raw_cmd_t *cmd, modbus_raw_res_t *res)
{
    if (!cmd || !res) return ESP_ERR_INVALID_ARG;
    if (!s_bus_mutex) return ESP_ERR_INVALID_STATE;

    memset(res, 0, sizeof(modbus_raw_res_t));

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(1500)) != pdTRUE) {
        snprintf(res->err_msg, sizeof(res->err_msg), "总线忙(超时)");
        return ESP_ERR_TIMEOUT;
    }

    uint8_t tx[32];
    size_t tx_len = 0;
    size_t expected_rx_min = 5;

    tx[0] = cmd->slave_id;
    tx[1] = cmd->func_code;
    tx[2] = (cmd->reg_addr >> 8) & 0xFF;
    tx[3] = cmd->reg_addr & 0xFF;

    if (cmd->func_code == 0x05 || cmd->func_code == 0x06) {
        tx[4] = (cmd->val_or_qty >> 8) & 0xFF;
        tx[5] = cmd->val_or_qty & 0xFF;
        uint16_t crc = bsp_rs485_calc_crc16(tx, 6);
        tx[6] = crc & 0xFF; tx[7] = (crc >> 8) & 0xFF;
        tx_len = 8;
        expected_rx_min = 8;
    } else if (cmd->func_code == 0x0F) {
        tx[4] = 0x00; tx[5] = 0x04;
        tx[6] = 0x01; tx[7] = cmd->val_or_qty & 0xFF;
        uint16_t crc = bsp_rs485_calc_crc16(tx, 8);
        tx[8] = crc & 0xFF; tx[9] = (crc >> 8) & 0xFF;
        tx_len = 10;
        expected_rx_min = 8;
    } else {
        tx[4] = (cmd->val_or_qty >> 8) & 0xFF;
        tx[5] = cmd->val_or_qty & 0xFF;
        uint16_t crc = bsp_rs485_calc_crc16(tx, 6);
        tx[6] = crc & 0xFF; tx[7] = (crc >> 8) & 0xFF;
        tx_len = 8;
        expected_rx_min = 5;
    }

    res->tx_len = tx_len;
    memcpy(res->tx_buf, tx, tx_len);

    flush_rx_fifo();
    bsp_rs485_send(tx, tx_len);

    int recv_len = bsp_rs485_recv(res->rx_buf, sizeof(res->rx_buf), 400);
    res->rx_len = (recv_len > 0) ? (size_t)recv_len : 0;

    if (recv_len >= expected_rx_min) {
        uint16_t calc_crc = bsp_rs485_calc_crc16(res->rx_buf, recv_len - 2);
        uint16_t frame_crc = res->rx_buf[recv_len - 2] | (res->rx_buf[recv_len - 1] << 8);
        if (calc_crc == frame_crc) {
            trigger_485_led();
            res->success = true;
            snprintf(res->err_msg, sizeof(res->err_msg), "应答成功");
        } else {
            res->success = false;
            snprintf(res->err_msg, sizeof(res->err_msg), "CRC 校验失败");
        }
    } else {
        res->success = false;
        snprintf(res->err_msg, sizeof(res->err_msg), "超时/从机无应答");
    }

    xSemaphoreGive(s_bus_mutex);
    return res->success ? ESP_OK : ESP_FAIL;
}

/* ====================================================================
 * 后台综合轮询任务
 * ==================================================================== */

static void rs485_poll_task(void *pvParameters)
{
    ESP_LOGI(TAG, "485 万能引擎轮询任务就绪");

    while (1) {
        // 如果用户关闭了后台自动轮询，则静默休眠，不发出任何干扰报文，灯绝不乱闪！
        if (!s_auto_poll_enabled) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // 检查是否有任何启用的点位
        bool has_active = false;
        for (int i = 0; i < MAX_RS485_POINTS; i++) {
            if (s_points[i].enabled) { has_active = true; break; }
        }
        if (!has_active) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 依次轮询各点位
        for (int i = 0; i < MAX_RS485_POINTS; i++) {
            if (!s_points[i].enabled) continue;
            if (!s_auto_poll_enabled) break;

            if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
                continue;
            }

            rs485_point_t *pt = &s_points[i];
            uint8_t req[8];
            req[0] = pt->slave_id;
            req[2] = (pt->reg_addr >> 8) & 0xFF;
            req[3] = pt->reg_addr & 0xFF;

            uint8_t expected_len = 0;

            if (pt->kind == POINT_KIND_COIL_SWITCH) {
                // 读单线圈状态 (01码)
                req[1] = 0x01;
                req[4] = 0x00; req[5] = 0x01;
                expected_len = 6;
            } else if (pt->kind == POINT_KIND_DISCRETE_INPUT) {
                // 读离散输入状态 (02码)
                req[1] = 0x02;
                req[4] = 0x00; req[5] = 0x01;
                expected_len = 6;
            } else {
                // 读寄存器 (03 或 04码)
                req[1] = (pt->func_code == 4) ? 0x04 : 0x03;
                uint16_t reg_count = (pt->data_type == POINT_TYPE_UINT32 || pt->data_type == POINT_TYPE_FLOAT) ? 2 : 1;
                req[4] = (reg_count >> 8) & 0xFF;
                req[5] = reg_count & 0xFF;
                expected_len = 3 + reg_count * 2 + 2;
            }

            uint16_t crc = bsp_rs485_calc_crc16(req, 6);
            req[6] = crc & 0xFF;
            req[7] = (crc >> 8) & 0xFF;

            flush_rx_fifo();
            bsp_rs485_send(req, 8);

            uint8_t resp[32];
            int recv_len = bsp_rs485_recv(resp, expected_len, 200);

            if (recv_len >= expected_len && resp[0] == pt->slave_id && resp[1] == req[1]) {
                uint16_t resp_crc = bsp_rs485_calc_crc16(resp, recv_len - 2);
                uint16_t expected_crc = resp[recv_len - 2] | (resp[recv_len - 1] << 8);

                if (resp_crc == expected_crc) {
                    trigger_485_led();
                    pt->is_online = true;
                    pt->last_sync_time = (uint32_t)(esp_timer_get_time() / 1000000);

                    if (pt->kind == POINT_KIND_COIL_SWITCH || pt->kind == POINT_KIND_DISCRETE_INPUT) {
                        pt->current_state = (resp[3] & 0x01) ? true : false;
                    } else {
                        if (pt->data_type == POINT_TYPE_UINT16) {
                            uint16_t raw = (resp[3] << 8) | resp[4];
                            pt->current_val = (float)raw * pt->scale;
                        } else if (pt->data_type == POINT_TYPE_INT16) {
                            int16_t raw = (int16_t)((resp[3] << 8) | resp[4]);
                            pt->current_val = (float)raw * pt->scale;
                        } else if (pt->data_type == POINT_TYPE_UINT32) {
                            uint32_t raw = (resp[3] << 24) | (resp[4] << 16) | (resp[5] << 8) | resp[6];
                            pt->current_val = (float)raw * pt->scale;
                        } else if (pt->data_type == POINT_TYPE_FLOAT) {
                            union { uint32_t u; float f; } un;
                            un.u = (resp[3] << 24) | (resp[4] << 16) | (resp[5] << 8) | resp[6];
                            pt->current_val = un.f * pt->scale;
                        }
                    }
                } else {
                    pt->is_online = false;
                }
            } else {
                pt->is_online = false;
            }

            xSemaphoreGive(s_bus_mutex);
            vTaskDelay(pdMS_TO_TICKS(80));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t app_rs485_engine_init(uint32_t baud_rate)
{
    if (s_bus_mutex == NULL) {
        s_bus_mutex = xSemaphoreCreateMutex();
    }

    bsp_rs485_init(baud_rate);
    load_points_from_nvs();

    xTaskCreate(rs485_poll_task, "rs485_poll", 4096, NULL, 5, NULL);
    return ESP_OK;
}

rs485_point_t *app_rs485_engine_get_points(uint8_t *count)
{
    if (count) *count = MAX_RS485_POINTS;
    return s_points;
}

esp_err_t app_rs485_engine_set_point(uint8_t index, const rs485_point_t *point)
{
    if (index >= MAX_RS485_POINTS || !point) return ESP_ERR_INVALID_ARG;
    memcpy(&s_points[index], point, sizeof(rs485_point_t));
    save_points_to_nvs();
    return ESP_OK;
}

esp_err_t app_rs485_engine_delete_point(uint8_t index)
{
    if (index >= MAX_RS485_POINTS) return ESP_ERR_INVALID_ARG;
    memset(&s_points[index], 0, sizeof(rs485_point_t));
    save_points_to_nvs();
    return ESP_OK;
}
