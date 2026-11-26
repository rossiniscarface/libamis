// SPDX-License-Identifier: MIT
// Minimal ESP-IDF example: read the AMIS on UART1 (9600 8E1) and log the values. UART errors come as events:
// the block with the damaged byte is dropped (no E5), see README "UART errors".
#include <amis.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Defaults that avoid the flash, PSRAM and console pins of the dev kits; check them for your board
#ifndef AMIS_RX_PIN
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
#define AMIS_RX_PIN 5  // C3: 18-21 are USB/UART0, 12-17 flash; C6: 16/17 are UART0
#define AMIS_TX_PIN 4
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define AMIS_RX_PIN 18
#define AMIS_TX_PIN 17
#else
#define AMIS_RX_PIN 16  // classic ESP32 (not on WROVER modules: there 16/17 belong to the PSRAM)
#define AMIS_TX_PIN 17
#endif
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
  QueueHandle_t events = nullptr;
  ESP_ERROR_CHECK(uart_driver_install(port, 512, 0, 16, &events, 0));
  ESP_ERROR_CHECK(uart_param_config(port, &cfg));
  ESP_ERROR_CHECK(uart_set_pin(port, AMIS_TX_PIN, AMIS_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

  if (!reader.set_key_hex("00112233445566778899AABBCCDDEEFF")) {  // your own key from the Netz OOe portal
    ESP_LOGE(TAG, "invalid key");
  }
  reader.set_required_fields(amis::Reading::kAll);  // the AMIS always sends all values

  bool damaged = false;  // a UART error was reported: drop the next data block
  uart_event_t ev;
  uint8_t buf[128];
  for (;;) {
    if (xQueueReceive(events, &ev, portMAX_DELAY) != pdTRUE) continue;
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    switch (ev.type) {
      case UART_DATA: {
        size_t left = ev.size;
        while (left > 0) {
          const int n = uart_read_bytes(port, buf, left < sizeof(buf) ? left : sizeof(buf), 0);
          if (n <= 0) break;
          left -= static_cast<size_t>(n);
          if (damaged) continue;
          for (int i = 0; i < n; i++) {
            const uint8_t st = reader.push(buf[i], now_ms);
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
        if (damaged) {
          damaged = false;
          reader.push_error(now_ms);
        }
        break;
      }
      case UART_PARITY_ERR:
      case UART_FRAME_ERR:
        damaged = true;
        break;
      case UART_FIFO_OVF:
      case UART_BUFFER_FULL:
        uart_flush_input(port);
        xQueueReset(events);
        reader.push_error(now_ms);
        break;
      default:
        break;
    }
  }
}
