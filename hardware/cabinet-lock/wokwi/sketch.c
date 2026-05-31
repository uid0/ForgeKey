// ForgeKey cabinet-lock Wokwi GPIO smoke test.
// Uses ESP32-C3 in Wokwi as a logic-level stand-in for the ESP32-C6 lock board.
// Production hardware must use the MOSFET, flyback diode, and 12 V lock supply
// documented in ../pin-manifest.json and ../bom.md.

#include <stdio.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SOLENOID_GPIO GPIO_NUM_0
#define REED_GPIO GPIO_NUM_1
#define IR_GPIO GPIO_NUM_4
#define MORTISE_GPIO GPIO_NUM_5
#define LATCH_GPIO GPIO_NUM_6

static void configure_input(gpio_num_t pin) {
  gpio_config_t cfg = {
    .pin_bit_mask = 1ULL << pin,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
}

void app_main(void) {
  gpio_config_t out = {
    .pin_bit_mask = 1ULL << SOLENOID_GPIO,
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&out);
  configure_input(REED_GPIO);
  configure_input(IR_GPIO);
  configure_input(MORTISE_GPIO);
  configure_input(LATCH_GPIO);

  while (true) {
    bool reed_closed = gpio_get_level(REED_GPIO) == 0;
    bool ir_broken = gpio_get_level(IR_GPIO) == 0;
    bool mortise_active = gpio_get_level(MORTISE_GPIO) == 0;
    bool latch_locked = gpio_get_level(LATCH_GPIO) == 0;
    bool unlock_request = mortise_active || (reed_closed && !ir_broken && latch_locked);
    gpio_set_level(SOLENOID_GPIO, unlock_request ? 1 : 0);
    printf("reed=%d ir_broken=%d mortise=%d latch_locked=%d solenoid=%d\n",
           reed_closed, ir_broken, mortise_active, latch_locked, unlock_request);
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
