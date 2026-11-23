/**
 * @file    app_can.c
 * @brief   ESP32 TWAI(CAN) 应用层驱动实现。
 *
 * 本文件基于 ESP-IDF 的 esp_twai / esp_twai_onchip 组件，封装了一个简单的
 * CAN 收发接口，供上层业务代码调用。主要做了以下几件事：
 *   1. 在 app_can_init() 中创建并配置片上 TWAI 节点、接收队列、滤波器和回调；
 *   2. 在接收完成中断回调 can_rx_done_cb() 中把硬件收到的帧搬运到 FreeRTOS 队列；
 *   3. 在 app_can_send() 中把一帧数据交给 TWAI 节点发送；
 *   4. 在 app_can_receive() 中从队列里取出收到的帧。
 *
 * 设计要点：
 *   - 中断回调里只做“取帧 + 入队”的轻量工作，避免在 ISR 中做耗时操作；
 *   - 业务线程通过 app_can_receive() 从队列阻塞等待，从而实现中断与任务解耦；
 *   - 仅面向经典 CAN（Classic CAN），单帧数据最多 8 字节。
 */

#include <string.h>
#include "app_can.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"

/*
 * 硬件引脚与总线参数配置。
 * 注意：下面的 GPIO 编号需要与实际电路板接线保持一致。
 */
#define CAN_TX_GPIO     GPIO_NUM_16   // 按你的实际硬件修改：TWAI 发送引脚（连到收发器 TXD）
#define CAN_RX_GPIO     GPIO_NUM_17   // 按你的实际硬件修改：TWAI 接收引脚（连到收发器 RXD）
#define CAN_BITRATE     500000        // 500 kbps：CAN 总线波特率，双方必须一致
#define CAN_RX_QUEUE_LEN 20           // 接收队列深度：最多缓存 20 帧待处理报文

/* 日志标签，统一用于 ESP_LOGx 输出，方便过滤本模块日志 */
static const char *TAG = "APP_CAN";

/* TWAI 节点句柄：由 app_can_init() 创建，NULL 表示尚未初始化 */
static twai_node_handle_t s_can_node = NULL;
/* 接收队列句柄：ISR 与任务之间传递 app_can_msg_t 的 FreeRTOS 队列 */
static QueueHandle_t s_rx_queue = NULL;

/**
 * @brief 接收完成中断回调：从硬件取出一帧并放入 FreeRTOS 队列。
 *
 * 该函数运行在中断上下文（由 IRAM_ATTR 保证放在内部 RAM 中执行），
 * 因此必须保持简短，不能调用可能阻塞的 API。
 *
 * @param handle  触发回调的 TWAI 节点句柄
 * @param edata   事件附带数据（本实现暂未使用）
 * @param user_ctx 用户上下文（本实现注册时传 NULL，未使用）
 * @return true  表示唤醒了更高优先级任务，需要触发一次上下文切换
 * @return false 表示无需切换
 */
static bool IRAM_ATTR can_rx_done_cb(twai_node_handle_t handle,
                                     const twai_rx_done_event_data_t *edata,
                                     void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;      // 标记是否唤醒了高优先级任务，初值为“否”
    uint8_t rx_buf[8] = {0};                    // 经典 CAN 单帧最大 8 字节，缓冲区清零
    /* 构造接收帧描述：告诉驱动把数据写入 rx_buf，容量为 8 字节 */
    twai_frame_t rx_frame = {
        .buffer = rx_buf,
        .buffer_len = sizeof(rx_buf),
    };

    /* 从硬件接收 FIFO 中读取一帧；成功才继续处理 */
    if (twai_node_receive_from_isr(handle, &rx_frame) == ESP_OK) {
        /* 队列已创建时才入队，避免空句柄操作 */
        if (s_rx_queue != NULL) {
            /* 把底层帧信息转换为应用层的统一消息结构 */
            app_can_msg_t msg = {
                .id = rx_frame.header.id,                          // 报文标识符（11 位或 29 位）
                .len = (uint8_t)twaifd_dlc2len(rx_frame.header.dlc), // DLC 编码转换为实际字节数
                .is_ext = (rx_frame.header.ide != 0),              // IDE 非 0 表示扩展帧
            };
            /* 防御性检查：实际长度不能超过应用层缓冲区大小 */
            if (msg.len > sizeof(msg.data)) {
                msg.len = sizeof(msg.data);
            }
            /* 拷贝有效数据到消息体 */
            memcpy(msg.data, rx_buf, msg.len);
            /* 从 ISR 入队；队列满时本条消息被丢弃（不阻塞中断） */
            xQueueSendFromISR(s_rx_queue, &msg, &high_task_wakeup);
        }
    }
    /* 若入队唤醒了等待中的任务，返回 true 请求调度器切换 */
    return (high_task_wakeup == pdTRUE);
}

/**
 * @brief 总线错误回调。
 *
 * 当 TWAI 控制器检测到总线错误（如位错误、ACK 错误、总线关闭等）时被调用。
 * 当前实现仅打印错误标志，便于调试；返回 false 表示不需要唤醒任务。
 *
 * @param handle  触发回调的 TWAI 节点句柄
 * @param edata   错误事件数据，err_flags.val 为错误标志位
 * @param user_ctx 用户上下文（未使用）
 * @return false  固定返回 false，不触发任务切换
 */
static bool IRAM_ATTR can_on_error_cb(twai_node_handle_t handle,
                                      const twai_error_event_data_t *edata,
                                      void *user_ctx)
{
    /* 在中断上下文使用 EARLY 版本日志，避免运行时开销较大的普通日志接口 */
    ESP_EARLY_LOGW(TAG, "TWAI bus error: 0x%x", edata->err_flags.val);
    return false;
}

/**
 * @brief 初始化 CAN(TWAI) 模块。
 *
 * 完成以下步骤：
 *   1. 幂等检查：若节点已存在则直接返回成功；
 *   2. 创建接收队列；
 *   3. 配置引脚、波特率、重试次数、发送队列深度等节点参数；
 *   4. 创建片上 TWAI 节点；
 *   5. 配置全放行滤波器（接收所有标准帧/扩展帧）；
 *   6. 注册接收完成与错误回调；
 *   7. 使能节点，开始收发。
 *
 * @return ESP_OK 成功；其他 esp_err_t 表示对应步骤失败。
 */
esp_err_t app_can_init(void)
{
    /* 幂等保护：已经初始化过就直接返回，避免重复创建资源 */
    if (s_can_node != NULL) {
        return ESP_OK;
    }

    /* 创建接收队列：每个元素是一帧应用层消息 app_can_msg_t */
    s_rx_queue = xQueueCreate(CAN_RX_QUEUE_LEN, sizeof(app_can_msg_t));
    /* 队列创建失败通常意味着内存不足，直接返回错误 */
    ESP_RETURN_ON_FALSE(s_rx_queue != NULL, ESP_ERR_NO_MEM, TAG, "Create rx queue failed");

    /* 片上 TWAI 节点配置 */
    twai_onchip_node_config_t node_config = {
        .io_cfg = {
            .tx = CAN_TX_GPIO,                 // 发送引脚
            .rx = CAN_RX_GPIO,                 // 接收引脚
            .quanta_clk_out = GPIO_NUM_NC,     // 不输出采样时钟（未使用）
            .bus_off_indicator = GPIO_NUM_NC,  // 不接总线关闭指示引脚（未使用）
        },
        .bit_timing = {
            .bitrate = CAN_BITRATE,            // 设置总线波特率（500 kbps）
        },
        .fail_retry_cnt = 3,                   // 发送失败时的自动重试次数
        .tx_queue_depth = 10,                  // 驱动内部发送队列深度
        // 如果要无收发器单板自测，可取消下面两行注释：
        // .flags.enable_self_test = 1,        // 开启自测模式（内部回环，无需外部收发器）
        // .flags.enable_loopback = 1,         // 开启回环模式（发送的帧也能被自己接收）
    };

    /* 创建片上 TWAI 节点，成功后 s_can_node 持有节点句柄 */
    ESP_RETURN_ON_ERROR(twai_new_node_onchip(&node_config, &s_can_node),
                        TAG, "Create TWAI node failed");

    // 接收所有标准帧与扩展帧（全放行滤波器）
    /*
     * 掩码滤波器原理：满足 (frame_id & mask) == (filter_id & mask) 的帧才被接收。
     * 这里 id=0、mask=0，条件恒成立，因此等价于“接收所有帧”。
     */
    twai_mask_filter_config_t filter_cfg = {
        .id = 0,          // 滤波器 ID 为 0
        .mask = 0,        // 掩码为 0，表示不比较任何位（全放行）
        .is_ext = false,  // 该滤波器针对标准帧配置
    };
    /* 在滤波器组 0 上应用上述配置 */
    ESP_RETURN_ON_ERROR(twai_node_config_mask_filter(s_can_node, 0, &filter_cfg),
                        TAG, "Config filter failed");

    /* 注册事件回调：接收完成与总线错误 */
    twai_event_callbacks_t cbs = {
        .on_rx_done = can_rx_done_cb,   // 每收到一帧触发
        .on_error = can_on_error_cb,    // 发生总线错误时触发
    };
    /* 第三个参数为回调用户上下文，这里不需要，传 NULL */
    ESP_RETURN_ON_ERROR(twai_node_register_event_callbacks(s_can_node, &cbs, NULL),
                        TAG, "Register callbacks failed");

    /* 使能节点，之后即可正常收发报文 */
    ESP_RETURN_ON_ERROR(twai_node_enable(s_can_node),
                        TAG, "Enable TWAI node failed");

    /* 打印初始化成功信息，便于确认引脚与波特率配置 */
    ESP_LOGI(TAG, "CAN initialized (TX=%d, RX=%d, bitrate=%d)",
             CAN_TX_GPIO, CAN_RX_GPIO, CAN_BITRATE);
    return ESP_OK;
}

/**
 * @brief 发送一帧 CAN 数据（标准帧/扩展帧，最多 8 字节）。
 *
 * @param id     报文标识符（标准帧 11 位，扩展帧 29 位）
 * @param data   数据指针，可为 NULL（此时发送长度为 0 的空帧）
 * @param len    数据长度，范围 0~8
 * @param is_ext true 发送扩展帧，false 发送标准帧
 * @return ESP_OK 成功；ESP_ERR_INVALID_STATE 未初始化；
 *         ESP_ERR_INVALID_ARG 参数非法；其他为驱动发送错误。
 */
esp_err_t app_can_send(uint32_t id, const uint8_t *data, uint8_t len, bool is_ext)
{
    /* 前置检查：必须先初始化 */
    ESP_RETURN_ON_FALSE(s_can_node != NULL, ESP_ERR_INVALID_STATE, TAG, "CAN not initialized");
    /* 前置检查：经典 CAN 单帧数据长度不能超过 8 字节 */
    ESP_RETURN_ON_FALSE(len <= 8, ESP_ERR_INVALID_ARG, TAG, "Classic CAN max len is 8");

    uint8_t tx_buf[8] = {0};   // 发送缓冲区，清零以保证未使用字节为 0
    /* 有有效数据时才拷贝，避免对 NULL 指针解引用 */
    if (data != NULL && len > 0) {
        memcpy(tx_buf, data, len);
    }

    /* 构造发送帧：头部信息 + 数据缓冲区 */
    twai_frame_t tx_frame = {
        .header = {
            .id = id,                  // 报文 ID
            .ide = is_ext ? 1 : 0,     // IDE 位：1 表示扩展帧，0 表示标准帧
        },
        .buffer = tx_buf,              // 指向待发送数据
        .buffer_len = len,             // 实际发送字节数
    };

    /* 提交发送，超时 500 ms；返回驱动的发送结果 */
    return twai_node_transmit(s_can_node, &tx_frame, 500);
}

/**
 * @brief 从接收队列读取一帧 CAN 数据。
 *
 * 由业务任务调用，若队列为空则最多阻塞 timeout_ms 毫秒。
 *
 * @param msg        输出参数，接收到的帧写入此结构
 * @param timeout_ms 等待超时时间（毫秒），0 表示不等待
 * @return true  成功取到一帧；false 超时、参数非法或模块未初始化
 */
bool app_can_receive(app_can_msg_t *msg, uint32_t timeout_ms)
{
    /* 参数与状态校验：队列未创建或输出指针为空都直接失败 */
    if (s_rx_queue == NULL || msg == NULL) {
        return false;
    }
    /* 阻塞等待队列数据，毫秒转换为 FreeRTOS tick；pdTRUE 表示取到数据 */
    return xQueueReceive(s_rx_queue, msg, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
