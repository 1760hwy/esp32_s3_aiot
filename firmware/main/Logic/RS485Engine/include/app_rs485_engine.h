#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_RS485_POINTS  24 // 最大支持同时配置 24 个万能点位

// 点位角色类型 (涵盖工业控制与采集的所有常见形态)
typedef enum {
    POINT_KIND_COIL_SWITCH    = 0, // 开关控制型: 对应 05写单线圈 / 01读线圈 (如各路继电器、接触器)
    POINT_KIND_DISCRETE_INPUT = 1, // 状态监测型: 对应 02读离散输入 (如光耦输入、行程开关、干接点)
    POINT_KIND_ANALOG_INPUT   = 2, // 模拟量看板型: 对应 03/04读寄存器 (如温度、湿度、电压、电流、功率)
    POINT_KIND_HOLDING_WRITE  = 3, // 数值设定型: 对应 06写单寄存器 / 03读回显 (如变频器调速、目标设定值)
} rs485_point_kind_t;

// 数据转换类型 (针对模拟量/寄存器)
typedef enum {
    POINT_TYPE_UINT16 = 0, // 16 位无符号整数
    POINT_TYPE_INT16  = 1, // 16 位有符号整数
    POINT_TYPE_UINT32 = 2, // 32 位无符号整数
    POINT_TYPE_FLOAT  = 3  // 32 位浮点数 (IEEE 754)
} rs485_data_type_t;

// 万能物模型点位结构体
typedef struct {
    bool enabled;                  // 是否启用
    char name[28];                 // 点位名称，如 "1号继电器", "进线电压", "变频器频率"
    char unit[10];                 // 单位，如 "℃", "%RH", "V", "Hz"
    rs485_point_kind_t kind;       // 点位形态 (开关控制 / 光耦监测 / 模拟量看板 / 数值设定)
    uint8_t slave_id;              // Modbus 从机地址 (1~254)
    uint8_t func_code;             // 主功能码 (01/02/03/04/05/06)
    uint16_t reg_addr;             // 寄存器地址 (0x0000 ~ 0xFFFF)
    
    // 模拟量属性
    rs485_data_type_t data_type;   // 数据类型
    float scale;                   // 乘积倍率 (如 0.1)

    // 控制与运行态数据
    bool current_state;            // 开关型/监测型的当前布尔状态 (true:闭合/导通, false:断开)
    float current_val;             // 模拟量/设定型的当前物理值
    bool is_online;                // 该点位是否正常在线 (上一次通信是否应答)
    uint32_t last_sync_time;       // 上次更新时间
} rs485_point_t;

// 预设模板类型枚举
typedef enum {
    TEMPLATE_DAM0404D     = 0, // 聚英 DAM0404D (4路继电器 + 4路光耦DI)
    TEMPLATE_RELAY_8CH    = 1, // 通用 8 路 Modbus 继电器板
    TEMPLATE_SHT30_ENV    = 2, // 标准 Modbus 温湿度传感器
    TEMPLATE_VFD_BASIC    = 3, // 变频器通用控制 (启停控制 + 频率调速)
} rs485_template_type_t;

/* ====================================================================
 * 引擎控制与点位操作 API
 * ==================================================================== */

/**
 * @brief 初始化 RS-485 万能引擎
 */
esp_err_t app_rs485_engine_init(uint32_t baud_rate);

/**
 * @brief 获取所有点位列表
 */
rs485_point_t *app_rs485_engine_get_points(uint8_t *count);

/**
 * @brief 设置/修改单个点位
 */
esp_err_t app_rs485_engine_set_point(uint8_t index, const rs485_point_t *point);

/**
 * @brief 删除单个点位
 */
esp_err_t app_rs485_engine_delete_point(uint8_t index);

/**
 * @brief 一键应用预设设备模板 (覆盖生成整套点位)
 */
esp_err_t app_rs485_engine_apply_template(rs485_template_type_t template_type, uint8_t slave_addr);

/**
 * @brief 清空所有点位
 */
esp_err_t app_rs485_engine_clear_all_points(void);

/**
 * @brief 前台即时控制接口：控制开关型点位 (功能码 05)
 * @param index 点位索引 0 ~ (MAX_RS485_POINTS - 1)
 * @param state true: 吸合/开启, false: 断开/关闭
 */
esp_err_t app_rs485_engine_control_switch(uint8_t index, bool state);

/**
 * @brief 前台即时控制接口：设定数值型点位 (功能码 06)
 * @param index 点位索引
 * @param val 要写入的数值
 */
esp_err_t app_rs485_engine_write_value(uint8_t index, float val);

/**
 * @brief 一键全部开关控制 (向所有启用的开关点位发送全部开启/全部关闭)
 */
esp_err_t app_rs485_engine_control_all_switches(bool state);

/**
 * @brief 设置后台自动轮询开关
 */
void app_rs485_engine_set_auto_poll(bool enable);
bool app_rs485_engine_is_auto_poll(void);

/**
 * @brief 获取 485 595 指示灯状态 (闪烁脉冲)
 */
bool app_rs485_engine_get_led_state(void);


/* ====================================================================
 * 万能 Modbus 调试助手
 * ==================================================================== */

typedef struct {
    uint8_t slave_id;
    uint8_t func_code;
    uint16_t reg_addr;
    uint16_t val_or_qty;
} modbus_raw_cmd_t;

typedef struct {
    bool success;
    uint8_t tx_buf[32];
    size_t tx_len;
    uint8_t rx_buf[64];
    size_t rx_len;
    char err_msg[32];
} modbus_raw_res_t;

esp_err_t app_rs485_engine_execute_raw(const modbus_raw_cmd_t *cmd, modbus_raw_res_t *res);

#ifdef __cplusplus
}
#endif
