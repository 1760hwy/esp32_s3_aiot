#include "app_DO.h"
#include "driver/gpio.h"

void bsp_do_init(void)
{
    gpio_set_level(DO1_PIN,0);
    gpio_set_level(DO2_PIN,0);
    gpio_set_level(DO3_PIN,0);
    gpio_set_level(DO4_PIN,0);

    gpio_config_t io_conf={
        .pin_bit_mask=DO_PINS_MASK,
        .mode=GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en=GPIO_PULLUP_DISABLE,
        .pull_down_en=GPIO_PULLDOWN_DISABLE,
        .intr_type=GPIO_INTR_DISABLE,
    };

    gpio_config(&io_conf);
    
}

void bsp_do_set(uint8_t channel,bool enable)
{
    gpio_num_t pin;

    switch (channel) {
        case 1: pin = DO1_PIN; break;
        case 2: pin = DO2_PIN; break;
        case 3: pin = DO3_PIN; break;
        case 4: pin = DO4_PIN; break;
        default: return;
    }

    gpio_set_level(pin,enable?1:0);
}

bool bsp_do_get(uint8_t channel)
{
    gpio_num_t pin;
    switch (channel) {
        case 1: pin = DO1_PIN; break;
        case 2: pin = DO2_PIN; break;
        case 3: pin = DO3_PIN; break;
        case 4: pin = DO4_PIN; break;
        default: return false;
    }
    return (gpio_get_level(pin) == 1);
}

void bsp_do_set_all(bool enable)
{
    for (int i = 0; i <= 4; i++)
    {
        /* code */
        bsp_do_set(i,enable);
    }
    
}