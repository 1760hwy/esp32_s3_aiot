#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief 初始化整个 DIDO 与 595 的联动模块，并启动后台 DI 扫描任务
 */
void app_dodi595_init(void);

/**
 * @brief 联动控制 DO：设置 DO 硬件输出，并自动同步更新 595 上的对应 LED
 * @param ch    通道号 0~3 (分别代表 DO1 ~ DO4)
 * @param state true: 打开 (GPIO输出高, LED亮); false: 关闭 (GPIO输出低, LED灭)
 */
void app_dodi595_set_do(uint8_t ch, bool state);

/**
 * @brief 获取当前某路 DO 的状态
 */
bool app_dodi595_get_do_status(uint8_t ch);

/**
 * @brief 获取当前某路 DI 的状态 (经过消抖后的稳定状态)
 */
bool app_dodi595_get_di_status(uint8_t ch);