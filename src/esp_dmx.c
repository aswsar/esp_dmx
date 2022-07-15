#include "esp_dmx.h"

#include <math.h>
#include <string.h>

#include "dmx_types.h"
#include "driver/gpio.h"
#include "driver/periph_ctrl.h"
#include "driver/uart.h"
#include "endian.h"
#include "esp_check.h"
#include "esp_log.h"
#include "impl/dmx_hal.h"
#include "impl/driver.h"
#include "impl/intr_handlers.h"
#include "soc/io_mux_reg.h"
#include "soc/uart_periph.h"
#include "soc/uart_reg.h"

enum {
  DMX_UART_FULL_DEFAULT = 1,      // The default value for the RX FIFO full interrupt threshold.
  DMX_UART_EMPTY_DEFAULT = 8,     // The default value for the TX FIFO empty interrupt threshold.
  DMX_UART_TIMEOUT_DEFAULT = 45,  // The default value for the UART timeout interrupt.

  DMX_MEMORY_CAPABILITIES = (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
};


#define DMX_FUNCTION_NOT_SUPPORTED()                         \
  ESP_LOGE(TAG, "%s() is not supported on %s", __FUNCTION__, \
           CONFIG_IDF_TARGET);                               \
  return ESP_ERR_NOT_SUPPORTED;

static const char *TAG = "dmx";  // The log tagline for the file.

static void dmx_module_enable(dmx_port_t dmx_num) {
  portENTER_CRITICAL(&(dmx_context[dmx_num].spinlock));
  if (dmx_context[dmx_num].hw_enabled != true) {
    periph_module_enable(uart_periph_signal[dmx_num].module);
    if (dmx_num != CONFIG_ESP_CONSOLE_UART_NUM) {
      /* Workaround for ESP32C3: enable core reset before enabling UART module
      clock to prevent UART output garbage value. */
#if SOC_UART_REQUIRE_CORE_RESET
      uart_hal_set_reset_core(&(dmx_context[dmx_num].hal), true);
      periph_module_reset(uart_periph_signal[dmx_num].module);
      uart_hal_set_reset_core(&(dmx_context[dmx_num].hal), false);
#else
      periph_module_reset(uart_periph_signal[dmx_num].module);
#endif
    }
    dmx_context[dmx_num].hw_enabled = true;
  }
  portEXIT_CRITICAL(&(dmx_context[dmx_num].spinlock));
}

static void dmx_module_disable(dmx_port_t dmx_num) {
  portENTER_CRITICAL(&(dmx_context[dmx_num].spinlock));
  if (dmx_context[dmx_num].hw_enabled != false) {
    if (dmx_num != CONFIG_ESP_CONSOLE_UART_NUM) {
      periph_module_disable(uart_periph_signal[dmx_num].module);
    }
    dmx_context[dmx_num].hw_enabled = false;
  }
  portEXIT_CRITICAL(&(dmx_context[dmx_num].spinlock));
}

/// Driver Functions  #########################################################
esp_err_t dmx_driver_install(dmx_port_t dmx_num, dmx_config_t *dmx_config) {

  dmx_context_t *const hardware = &dmx_context[dmx_num];
  dmx_driver_t *driver;

  // Configure the UART hardware
  dmx_module_enable(dmx_num);
  dmx_hal_init(&hardware->hal);

  // Flush the hardware FIFOs
  dmx_hal_rxfifo_rst(&hardware->hal);
  dmx_hal_txfifo_rst(&hardware->hal);

  // Allocate the DMX driver dynamically
  driver = heap_caps_malloc(sizeof(dmx_driver_t), DMX_MEMORY_CAPABILITIES);
  if (driver == NULL) {
    ESP_LOGE(TAG, "DMX driver malloc error");
    return ESP_ERR_NO_MEM;
  }
  dmx_driver[dmx_num] = driver;

  // Allocate semaphores dynamically
  driver->written_semaphore = xSemaphoreCreateBinary();
  driver->ready_semaphore = xSemaphoreCreateBinary();
  if (driver->written_semaphore == NULL || driver->ready_semaphore == NULL) {
    ESP_LOGE(TAG, "DMX driver semaphore malloc error");
    return ESP_ERR_NO_MEM;
  }
  xSemaphoreGive(driver->written_semaphore);
  xSemaphoreGive(driver->ready_semaphore);

  // Initialize the driver buffer
  bzero(driver->buffer.data, DMX_MAX_PACKET_SIZE);
  driver->buffer.size = DMX_MAX_PACKET_SIZE;
  driver->buffer.head = 0;

  // Initialize driver state
  driver->dmx_num = dmx_num;
  driver->mode = DMX_MODE_READ;
  driver->rst_seq_hw = dmx_config->rst_seq_hw;
  driver->timer_idx = dmx_config->timer_idx;
  driver->status_flags = 0;
  // driver->uid = dmx_get_uid(); // TODO

  // TODO: reorganize these inits
  // driver->tx.rdm_tn = 0;

  // Initialize TX settings
  driver->tx.break_len = DMX_BREAK_LEN_US;
  driver->tx.mab_len = DMX_WRITE_MIN_MAB_LEN_US;

  // initialize driver rx variables
  driver->rx.intr_io_num = -1;
  driver->rx.break_len = -1;
  driver->rx.mab_len = -1;

  // Driver ISR is in IRAM so interrupt flags must include IRAM flag
  if (!(dmx_config->intr_alloc_flags & ESP_INTR_FLAG_IRAM)) {
    ESP_LOGI(TAG, "ESP_INTR_FLAG_IRAM flag not set, flag updated");
    dmx_config->intr_alloc_flags |= ESP_INTR_FLAG_IRAM;
  }

  // Install UART interrupt
  dmx_hal_disable_interrupt(&hardware->hal, DMX_ALL_INTR_MASK);
  dmx_hal_clear_interrupt(&hardware->hal, DMX_ALL_INTR_MASK);
  esp_intr_alloc(uart_periph_signal[dmx_num].irq, dmx_config->intr_alloc_flags,
                 &dmx_intr_handler, driver, &driver->uart_isr_handle);
  const dmx_intr_config_t dmx_intr_conf = {
      .rxfifo_full_threshold = DMX_UART_FULL_DEFAULT,
      .rx_timeout_threshold = DMX_UART_TIMEOUT_DEFAULT,
      .txfifo_empty_threshold = DMX_UART_EMPTY_DEFAULT,
  };
  dmx_intr_config(dmx_num, &dmx_intr_conf);

  // Enable UART read interrupt and set RTS low
  portENTER_CRITICAL(&hardware->spinlock);
  dmx_hal_enable_interrupt(&hardware->hal, DMX_INTR_RX_ALL);
  dmx_hal_set_rts(&hardware->hal, DMX_MODE_READ);
  portEXIT_CRITICAL(&hardware->spinlock);

  // Install hardware timer interrupt
  if (driver->rst_seq_hw != DMX_USE_BUSY_WAIT) {
    const timer_config_t timer_conf = {
        .divider = 80,  // 80MHz / 80 == 1MHz resolution timer
        .counter_dir = TIMER_COUNT_UP,
        .counter_en = false,
        .alarm_en = true,
        .auto_reload = true,
    };
    timer_init(dmx_config->rst_seq_hw, dmx_config->timer_idx, &timer_conf);
    timer_set_counter_value(dmx_config->rst_seq_hw, dmx_config->timer_idx, 0);
    timer_set_alarm_value(dmx_config->rst_seq_hw, dmx_config->timer_idx, 0);
    timer_isr_callback_add(dmx_config->rst_seq_hw, dmx_config->timer_idx,
                           dmx_timer_intr_handler, driver,
                           dmx_config->intr_alloc_flags);
    timer_enable_intr(dmx_config->rst_seq_hw, dmx_config->timer_idx);
  }

  return ESP_OK;
}

esp_err_t dmx_driver_delete(dmx_port_t dmx_num) {

  return ESP_OK;
}

bool dmx_is_driver_installed(dmx_port_t dmx_num) {
  return dmx_num < DMX_NUM_MAX && dmx_driver[dmx_num] != NULL;
}

esp_err_t dmx_set_mode(dmx_port_t dmx_num, dmx_mode_t dmx_mode) {
  // TODO: check args

  dmx_driver_t *const driver = dmx_driver[dmx_num];
  dmx_context_t *const hardware = &dmx_context[dmx_num];

  // Return if the the driver is in the correct mode already
  portENTER_CRITICAL(&hardware->spinlock);
  const dmx_mode_t current_mode = driver->mode;
  portEXIT_CRITICAL(&hardware->spinlock);
  if (current_mode == dmx_mode) {
    return ESP_OK;
  }

  // Ensure driver isn't currently transmitting DMX data
  portENTER_CRITICAL(&hardware->spinlock);
  const int driver_is_busy = driver->is_busy;
  portEXIT_CRITICAL(&hardware->spinlock);
  if (current_mode == DMX_MODE_WRITE && driver_is_busy) {
    return ESP_FAIL;
  }

  // Clear interrupts and set the mode
  dmx_hal_clear_interrupt(&hardware->hal, DMX_ALL_INTR_MASK);
  driver->mode = dmx_mode;

  if (dmx_mode == DMX_MODE_READ) {
    // Reset the UART read FIFO
    dmx_hal_rxfifo_rst(&hardware->hal);

    // Set RTS and enable UART interrupts
    portENTER_CRITICAL(&hardware->spinlock);
    dmx_hal_set_rts(&hardware->hal, DMX_MODE_READ);
    dmx_hal_enable_interrupt(&hardware->hal, DMX_INTR_RX_ALL);
    portEXIT_CRITICAL(&hardware->spinlock);
  } else {
    // Disable read interrupts
    portENTER_CRITICAL(&hardware->spinlock);
    dmx_hal_disable_interrupt(&hardware->hal, DMX_INTR_RX_ALL);
    portEXIT_CRITICAL(&hardware->spinlock);

    // Disable DMX sniffer if it is enabled
    if (driver->rx.intr_io_num != -1) {
      dmx_sniffer_disable(dmx_num);
    }

    // Reset the UART write FIFO
    dmx_hal_txfifo_rst(&hardware->hal);

    // Set RTS and enable UART interrupts
    portENTER_CRITICAL(&hardware->spinlock);
    dmx_hal_set_rts(&hardware->hal, DMX_MODE_WRITE);
    portEXIT_CRITICAL(&hardware->spinlock);
  }

  return ESP_OK;
}

esp_err_t dmx_get_mode(dmx_port_t dmx_num, dmx_mode_t *dmx_mode) {
  
  return ESP_OK;
}

esp_err_t dmx_sniffer_enable(dmx_port_t dmx_num, int intr_io_num) {

  return ESP_OK;
}

esp_err_t dmx_sniffer_disable(dmx_port_t dmx_num) {

  return ESP_OK;
}

bool dmx_is_sniffer_enabled(dmx_port_t dmx_num) {
  return dmx_is_driver_installed(dmx_num) &&
         dmx_driver[dmx_num]->rx.intr_io_num != -1;
}

/// Hardware Configuration  ###################################################
esp_err_t dmx_set_pin(dmx_port_t dmx_num, int tx_io_num, int rx_io_num,
                      int rts_io_num) {
  ESP_RETURN_ON_FALSE(dmx_num >= 0 && dmx_num < DMX_NUM_MAX,
                      ESP_ERR_INVALID_ARG, TAG, "dmx_num error");
  ESP_RETURN_ON_FALSE(tx_io_num < 0 || GPIO_IS_VALID_OUTPUT_GPIO(tx_io_num),
                      ESP_ERR_INVALID_ARG, TAG, "tx_io_num error");
  ESP_RETURN_ON_FALSE(rx_io_num < 0 || GPIO_IS_VALID_GPIO(rx_io_num),
                      ESP_ERR_INVALID_ARG, TAG, "rx_io_num error");
  ESP_RETURN_ON_FALSE(rts_io_num < 0 || GPIO_IS_VALID_OUTPUT_GPIO(rts_io_num),
                      ESP_ERR_INVALID_ARG, TAG, "rts_io_num error");

  return uart_set_pin(dmx_num, tx_io_num, rx_io_num, rts_io_num,
                      DMX_PIN_NO_CHANGE);
}

esp_err_t dmx_set_baud_rate(dmx_port_t dmx_num, uint32_t baud_rate) {

  return ESP_OK;
}

esp_err_t dmx_get_baud_rate(dmx_port_t dmx_num, uint32_t *baud_rate) {

  return ESP_OK;
}

esp_err_t dmx_set_break_len(dmx_port_t dmx_num, uint32_t break_len) {

  return ESP_OK;
}

esp_err_t dmx_get_break_len(dmx_port_t dmx_num, uint32_t *break_len) {


  return ESP_OK;
}

esp_err_t dmx_set_mab_len(dmx_port_t dmx_num, uint32_t mab_len) {

  return ESP_OK;
}

esp_err_t dmx_get_mab_len(dmx_port_t dmx_num, uint32_t *mab_len) {


  return ESP_OK;
}

/// Interrupt Configuration  ##################################################
esp_err_t dmx_intr_config(dmx_port_t dmx_num,
                          const dmx_intr_config_t *intr_conf) {
 
  return ESP_OK;
}

esp_err_t dmx_set_rx_full_threshold(dmx_port_t dmx_num, int threshold) {
  

  return ESP_OK;
}
esp_err_t dmx_set_tx_empty_threshold(dmx_port_t dmx_num, int threshold) {
  

  return ESP_OK;
}

esp_err_t dmx_set_rx_timeout(dmx_port_t dmx_num, uint8_t timeout) {
  
  return ESP_OK;
}

/// Read/Write  ###############################################################
esp_err_t dmx_read(dmx_port_t dmx_num, void *data, size_t size) {
  // TODO: Check arguments


  return ESP_OK;
}

esp_err_t dmx_write(dmx_port_t dmx_num, const void *data, size_t size) {
  // TODO: check args

  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Copy data from the source to the driver buffer
  // This is not synchronous - use dmx_wait_sent() for frame synchronization
  memcpy(driver->buffer.data, data, size);

  return ESP_OK;
}

esp_err_t dmx_read_slot(dmx_port_t dmx_num, size_t index, uint8_t *value) {

  return ESP_OK;
}

esp_err_t dmx_write_slot(dmx_port_t dmx_num, size_t index,
                         const uint8_t value) {
  return ESP_OK;
}

esp_err_t dmx_send_packet(dmx_port_t dmx_num, size_t size) {
  // TODO: Check arguments
  
  dmx_driver_t *const driver = dmx_driver[dmx_num];
  dmx_context_t *const hardware = &dmx_context[dmx_num];

  // Ensure driver isn't currently transmitting DMX data
  portENTER_CRITICAL(&hardware->spinlock);
  const int driver_is_busy = driver->is_busy;
  portEXIT_CRITICAL(&hardware->spinlock);
  if (driver_is_busy) {
    return ESP_FAIL;
  }

  // Set the driver busy flag and set the buffer state
  driver->is_busy = true;
  driver->buffer.size = size;
  driver->buffer.head = 0;

  // Get the configured length of the DMX break
  portENTER_CRITICAL(&hardware->spinlock);
  const uint32_t break_len = driver->tx.break_len;
  portEXIT_CRITICAL(&hardware->spinlock);

  // TODO: allow busy wait mode

  // Setup hardware timer for DMX break
  timer_set_counter_value(driver->rst_seq_hw, driver->timer_idx, 0);
  timer_set_alarm_value(driver->rst_seq_hw, driver->timer_idx, break_len);

  // Trigger the DMX break
  driver->is_in_break = true;
  dmx_hal_invert_signal(&hardware->hal, UART_SIGNAL_TXD_INV);
  timer_start(driver->rst_seq_hw, driver->timer_idx);

  return ESP_OK;
}

esp_err_t dmx_wait_packet_received(dmx_port_t dmx_num, dmx_event_t *event,
                                   TickType_t ticks_to_wait) {
  // TODO: Check arguments

  dmx_driver_t *const driver = dmx_driver[dmx_num];
  dmx_context_t *const hardware = &dmx_context[dmx_num];

  // Block task if required
  uint32_t flags;
  bool packet_received = false;
  if (ticks_to_wait > 0) {
    portENTER_CRITICAL(&hardware->spinlock);

    // Ensure only one task is calling this function
    if (driver->buffer.waiting_task != NULL) {
      portEXIT_CRITICAL(&hardware->spinlock);
      return ESP_FAIL;
    }

    // Ensure the driver notifies this task when a packet is received
    driver->buffer.waiting_task = xTaskGetCurrentTaskHandle();;

    portEXIT_CRITICAL(&hardware->spinlock);

    // Recheck to ensure that this is the task that will be notified
    if (driver->buffer.waiting_task) {
      packet_received = xTaskNotifyWait(0, ULONG_MAX, &flags, ticks_to_wait);
      driver->buffer.waiting_task = NULL;
    }
  } else {
    portENTER_CRITICAL(&hardware->spinlock);

    // If task is not blocked, driver must be idle to receive data
    if (!driver->is_busy) {
      packet_received = true;
      // TODO: add error flags to driver so that they can be read into &flags
    }

    portEXIT_CRITICAL(&hardware->spinlock);
  }

  // Do not process data if a packet was not received
  if (!packet_received) {
    return ESP_ERR_TIMEOUT;
  }

  // Handle packet processing
  ESP_LOGI(TAG, "We received a packet! Woohoo!"); // TODO

  return ESP_OK;
}

esp_err_t dmx_wait_packet_written(dmx_port_t dmx_num,
                                  TickType_t ticks_to_wait) {
  // TODO: Check arguments
  
  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Block until the DMX data has been sent
  if (!xSemaphoreTake(driver->written_semaphore, ticks_to_wait)) {
    return ESP_ERR_TIMEOUT;
  }
  xSemaphoreGive(driver->written_semaphore);

  return ESP_OK;
}

esp_err_t dmx_wait_send_ready(dmx_port_t dmx_num, TickType_t ticks_to_wait) {

  return ESP_OK;
}


void *memcpyswap(void *dest, const void *src, size_t n) {
  char *chrdest = (char *)dest;
  char *chrsrc = (char *)src + n - 1;
  for (; (void *)chrsrc >= src; --chrsrc, chrdest++) {
    *chrdest = *chrsrc;
  }
  return chrdest;
}



esp_err_t dmx_wait_turnaround(dmx_port_t dmx_num, TickType_t ticks_to_wait) {


  return ESP_OK;
}