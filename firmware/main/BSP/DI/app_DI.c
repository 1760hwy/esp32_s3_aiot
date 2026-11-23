#include "app_DI.h"
#include "driver/gpio.h"

void bsp_di_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = DI_PINS_MASK,         // 指定 DI1~DI4 引脚
        .mode = GPIO_MODE_INPUT,              // 纯输入模式
        .pull_up_en = GPIO_PULLUP_ENABLE,     // 开启芯片内部弱上拉 (板载已有 2k 上拉，双保险)
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    gpio_config(&io_conf);
}

bool bsp_di_get_raw(uint8_t channel)
{
    gpio_num_t pin;
    switch (channel) {
        case 1: pin = DI1_PIN; break;
        case 2: pin = DI2_PIN; break;
        case 3: pin = DI3_PIN; break;
        case 4: pin = DI4_PIN; break;
        default: return false;
    }

    // 光耦导通时 GPIO 读到 0，因此取反！
    // 读到 0 返回 true (有信号)，读到 1 返回 false (无信号)
    return (gpio_get_level(pin) == 0);
}