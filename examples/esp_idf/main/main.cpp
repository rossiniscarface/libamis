// SPDX-License-Identifier: MIT
// Minimal ESP-IDF example: read the AMIS on UART1 (9600 8E1) and log the values.
#include <amis.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"

#ifndef AMIS_RX_PIN
#define AMIS_RX_PIN 17
#endif
#ifndef AMIS_TX_PIN
#define AMIS_TX_PIN 16
#endif

static const char *TAG = "amis";
static amis::Reader reader;

extern "C" void app_main(void) {
  const uart_port_t port = UART_NUM_1;
  uart_config_t cfg = {};
  cfg.baud_rate = 9600;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_EVEN;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;
  ESP_ERROR_CHECK(uart_driver_install(port, 512, 0, 0, nullptr, 0));
  ESP_ERROR_CHECK(uart_param_config(port, &cfg));
  ESP_ERROR_CHECK(uart_set_pin(port, AMIS_TX_PIN, AMIS_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

  if (!reader.set_key_hex("00112233445566778899AABBCCDDEEFF")) {  // your own key from the Netz OOe portal
    ESP_LOGE(TAG, "invalid key");
  }

  uint8_t b;
  for (;;) {
    if (uart_read_bytes(port, &b, 1, pdMS_TO_TICKS(100)) != 1) continue;
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    const uint8_t st = reader.push(b, now_ms);
    if (st & amis::Reader::kSendAck) {
      const uint8_t ack = amis::Reader::kAck;
      uart_write_bytes(port, &ack, 1);  // acknowledge immediately
    }
    if (st & amis::Reader::kReading) {
      const amis::Reading &r = reader.reading();
      ESP_LOGI(TAG, "1.8.0=%lu Wh  2.8.0=%lu Wh  net=%ld W", (unsigned long)r.energy_import_wh,
               (unsigned long)r.energy_export_wh, (long)r.power_balance_w());
    }
    if (st & amis::Reader::kError) ESP_LOGW(TAG, "%s", amis::error_str(reader.last_error()));
  }
}
