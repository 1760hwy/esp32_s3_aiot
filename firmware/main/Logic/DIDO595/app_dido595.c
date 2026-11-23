#include "app_DI.h"
#include "app_DO.h" 
#include "595.h"
#include "driver/gpio.h"
#include "app_dido595.h"
#include "app_network595.h"
#include "app_rs485_engine.h"
#include "app_can_engine.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"


static const gpio_num_t DO_PINS[4] = {DO1_PIN, DO2_PIN, DO3_PIN, DO4_PIN};

void app_dodi595_sync(void)
{
    uint16_t led_val=0xFFFF;// 初始化

    for (int i = 0; i < 4; i++)
    {
        /* code */
        if (bsp_di_get_raw(i+1)==true)
        {
            /* code */
            led_val &=~(1<<i);
        }
    }

    for (int i = 0; i < 4; i++) {
        if (gpio_get_level(DO_PINS[i]) == 1) { // 读当前 DO 状态
            led_val &= ~(1 << (4 + i)); // 点亮 LED(5+i)
        }
    }

    if (app_network595_get_eth_state() == true)
    {
        led_val &= ~LED_BIT_ETH;
    }

    // 4. RS-485 收发指示灯联动 (LED11 闪烁)
    if (app_rs485_engine_get_led_state() == true)
    {
        led_val &= ~LED_BIT_RS485;
    }

    // 5. CAN 工业总线收发指示灯联动 (LED10 闪烁)
    if (app_can_engine_get_led_state() == true)
    {
        led_val &= ~LED_BIT_CAN;
    }

    led_74hc595_write(led_val);
}

static void dodi_task(void *pvParameters)
{
    while (1)
    {
        /* code */
        app_dodi595_sync();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
}


void app_dodi595_init(void)
{
    bsp_do_init();
    bsp_di_init();
    led_74hc595_init();

    xTaskCreate(dodi_task,"dodi595_task",2048,NULL,5,NULL);
}


