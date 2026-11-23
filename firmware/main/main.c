#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// 驱动层
#include "595.h"
#include "app_DO.h"
#include "app_DI.h"
#include "app_wifi.h"
#include "app.network.h"
#include "app_485.h"
#include "app_can.h"

// 业务逻辑层
#include "app_dido595.h"
#include "app_network595.h"
#include "app_webserver.h"
#include "app_rs485_engine.h"
#include "app_can_engine.h"

static const char *TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "==== ESP32-S3 工业智能网关启动 ====");

    // 1. 初始化 595 指示灯系统与 DIDO 监控
    app_network595_init();
    app_dodi595_init();

    // 2. 初始化无线 Wi-Fi (Flash 自动断电记忆重连)
    app_wifi_init();

    // 3. 启动 W5500 有线以太网 (支持 DHCP 与静态 IP 自定义)
    app_network_init();

    // 4. 启动通用 RS-485 Modbus 物模型采集引擎 (默认波特率 9600)
    app_rs485_engine_init(9600);

    // 5. 启动 CAN 工业通信与 STM32 联动业务引擎 (波特率 500kbps)
    app_can_engine_init();

    // 6. 启动统一的 Web 控制台服务 (端口 8080)
    if (app_webserver_start(8080) != ESP_OK) {
        ESP_LOGW(TAG, "Web 控制台未启动");
    }

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
