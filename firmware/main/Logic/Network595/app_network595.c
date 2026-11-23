#include "app_network595.h"
#include "595.h"
#include "esp_log.h"

static const char *TAG = "APP_NET595";

static bool s_eth_connected = false;

void app_network595_init(void)
{
    s_eth_connected = false;
    ESP_LOGI(TAG, "Network595 联动模块就绪 (ETH 默认熄灭)");
}

void app_network595_set_eth_state(bool is_connected)
{
    s_eth_connected = is_connected;
    ESP_LOGI(TAG, "以太网状态更新: %s -> 595 ETH灯 %s",
             is_connected ? "已连接(GOT_IP)" : "断开",
             is_connected ? "点亮" : "熄灭");
}

bool app_network595_get_eth_state(void)
{
    return s_eth_connected;
}