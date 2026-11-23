#include "595.h"
#include "esp_rom_sys.h"

void led_74hc595_init(void)
{
    gpio_reset_pin(LED_595_DATA_PIN);
    gpio_reset_pin(LED_595_CLK_PIN);
    gpio_reset_pin(LED_595_LATCH_PIN);

    gpio_config_t io_conf={
        .pin_bit_mask=(1ULL << LED_595_DATA_PIN) | 
                      (1ULL << LED_595_CLK_PIN)  |
                      (1ULL << LED_595_LATCH_PIN),
        .mode=GPIO_MODE_OUTPUT,
        .pull_up_en=GPIO_PULLUP_DISABLE,
        .pull_down_en=GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE  
    };

    gpio_config(&io_conf);

    gpio_set_level(LED_595_DATA_PIN,0);
    gpio_set_level(LED_595_CLK_PIN, 0);
    gpio_set_level(LED_595_LATCH_PIN, 0);

    led_74hc595_write(LED_595_ALL_OFF);
}

void led_74hc595_write(uint16_t data16)
{
    // 1. 移位前先拉低 LATCH 锁存脚
    gpio_set_level(LED_595_LATCH_PIN, 0);
    esp_rom_delay_us(5);
    // 2. 从最高位 (Bit 15) 到最低位 (Bit 0) 逐位送出
    // 先送出的 8 位会被挤入后级 U4，后送出的 8 位留在前级 U1
    for (int i = 15; i >= 0; i--) {
        uint32_t bit_val = (data16 >> i) & 1;
        // 放置数据
        gpio_set_level(LED_595_DATA_PIN, bit_val);
        esp_rom_delay_us(2);
        // 产生时钟上升沿 (0 -> 1)，芯片采样当前数据位
        gpio_set_level(LED_595_CLK_PIN, 1);
        esp_rom_delay_us(5);
        // 时钟拉低，准备下一次上升沿
        gpio_set_level(LED_595_CLK_PIN, 0);
        esp_rom_delay_us(2);
    }
    // 3. 16 位移位全部完毕，产生锁存上升沿 (0 -> 1)
    // 内部移位数据瞬间被刷入输出引脚 (QA~QH)
    esp_rom_delay_us(5);
    gpio_set_level(LED_595_LATCH_PIN, 1);
    esp_rom_delay_us(5);
    gpio_set_level(LED_595_LATCH_PIN, 0);
}

/**
 * @brief 仅点亮指定的单个指示灯 (0~10)，其余熄灭
 */
void led_74hc595_set_single(uint8_t bit_index)
{
    if (bit_index > 10) 
        return;
    
    // 仅将第 bit_index 位置 0，其余位置 1
    uint16_t state = ~(1 << bit_index);
    led_74hc595_write(state);
}