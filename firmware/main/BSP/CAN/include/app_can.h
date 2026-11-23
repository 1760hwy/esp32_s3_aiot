#pragma once 

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
#endif

typedef struct 
{
    /* data */
    uint32_t id;
    uint8_t len;
    bool is_ext;
    uint8_t data[8];
} app_can_msg_t;


esp_err_t app_can_init(void);

/**
 * @brief 发送一帧 CAN 数据（标准帧/扩展帧，最多 8 字节）
 */
esp_err_t app_can_send(uint32_t id,const uint8_t *data,uint8_t len,bool is_ext);

/**
 * @brief 从接收队列读取一帧 CAN 数据
 */
bool app_can_receive(app_can_msg_t *msg, uint32_t timeout_ms);
#ifdef __cplusplus
}
#endif