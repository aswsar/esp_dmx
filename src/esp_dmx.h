#pragma once

#include "dmx_caps.h"
#include "dmx_types.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "soc/soc_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DMX_NUM_MAX SOC_UART_NUM  // DMX port max. Used for error checking.

enum {
  DMX_NUM_0,  // DMX port 0.
  DMX_NUM_1,  // DMX port 1.
#if DMX_NUM_MAX > 2
  DMX_NUM_2,  // DMX port 2.
#endif
};

enum {
  // Constant for dmx_set_pin(). Indicates the pin should not be changed.
  DMX_PIN_NO_CHANGE = -1
};

/**
 * @brief The default configuration for DMX. This macro may be used to
 * initialize a dmx_config_t to the standard's defined typical values.
 */
#define DMX_DEFAULT_CONFIG \
  { .rst_seq_hw = 0, .timer_idx = 0, .intr_alloc_flags = ESP_INTR_FLAG_IRAM }

/// Driver Functions  #########################################################
/**
 * @brief Install DMX driver and set the DMX to the default configuration. DMX
 * ISR handler will be attached to the same CPU core that this function is
 * running on. The default configuration sets the DMX break to 176 microseconds
 * and the DMX mark-after-break to 12 microseconds.
 *
 * @param dmx_num The DMX port number.
 * @param[in] dmx_config A pointer to a dmx_config_t.
 * @param queue_size The size of the DMX event queue.
 * @param[in] dmx_queue Handle to the event queue.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if there is an argument error.
 * @retval ESP_ERR_NO_MEM if there is not enough memory.
 * @retval ESP_ERR_INVALID_STATE if the driver already installed.
 * */
esp_err_t dmx_driver_install(dmx_port_t dmx_num, dmx_config_t *dmx_config);

/**
 * @brief Uninstall the DMX driver.
 *
 * @param dmx_num The DMX port number
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if there is an argument error.
 */
esp_err_t dmx_driver_delete(dmx_port_t dmx_num);

/**
 * @brief Checks if DMX driver is installed.
 *
 * @param dmx_num The DMX port number.
 * @retval true if the driver is installed.
 * @retval false if the driver is not installed or DMX port does not exist.
 * */
bool dmx_is_driver_installed(dmx_port_t dmx_num);

/**
 * @brief Enable the DMX sniffer to determine the break and mark-after-break
 * length.
 *
 * @note The sniffer uses the default GPIO ISR handler, which allows for many
 * ISRs to be registered to different GPIO pins. Depending on how many GPIO
 * interrupts are registered, there could be significant latency between when
 * the analyzer ISR runs and when an ISR condition actually occurs. A quirk of
 * this implementation is that ISRs are handled from lowest GPIO number to
 * highest. It is therefore recommended that the user shorts the UART rx pin to
 * the lowest numbered GPIO possible and enables the sniffer interrupt on that
 * pin to ensure that the analyzer ISR is called with the lowest latency
 * possible.
 *
 * @param dmx_num The DMX port number.
 * @param intr_io_num The pin to assign the to which to assign the interrupt.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if there was an argument error.
 * @retval ESP_ERR_INVALID_STATE if the driver is not installed, no queue, or
 * sniffer already enabled.
 */
esp_err_t dmx_sniffer_enable(dmx_port_t dmx_num, int intr_io_num);

/**
 * @brief Disable the DMX sniffer.
 *
 * @param dmx_num The DMX port number.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if there was an argument error.
 * @retval ESP_ERR_INVALID_STATE if the driver is not installed, no queue, or
 * sniffer already disabled.
 */
esp_err_t dmx_sniffer_disable(dmx_port_t dmx_num);

/**
 * @brief Checks if the sniffer is enabled.
 *
 * @param dmx_num The DMX port number.
 * @retval true if the sniffer is installed.
 * @retval false if the sniffer is not installed or DMX port does not exist.
 */
bool dmx_is_sniffer_enabled(dmx_port_t dmx_num);

/// Hardware Configuration  ###################################################
/**
 * @brief Set DMX pin number.
 *
 * @param dmx_num The DMX port number.
 * @param tx_io_num The pin to which the TX signal will be assigned.
 * @param rx_io_num The pin to which the RX signal will be assigned.
 * @param rts_io_num The pin to which the RTS signal will be assigned.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if there was an argument error.
 * */
esp_err_t dmx_set_pin(dmx_port_t dmx_num, int tx_io_num, int rx_io_num,
                      int rts_io_num);

/**
 * @brief Set the DMX baud rate.
 *
 * @note The baud rate must be within DMX specification. The lowest value is
 * DMX_MIN_BAUD_RATE and the highest value is DMX_MAX_BAUD_RATE (inclusive). The
 * macro DMX_BAUD_RATE_IS_VALID() may be used to check if a baud rate is to DMX
 * specification.
 *
 * @param dmx_num The DMX port number.
 * @param baud_rate The baud rate to set the DMX port to.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if there was an argument error.
 */
esp_err_t dmx_set_baud_rate(dmx_port_t dmx_num, uint32_t baud_rate);

/**
 * @brief Get the DMX baud rate.
 *
 * @param dmx_num The DMX port number.
 * @param[out] baud_rate The baud rate returned from the DMX configuration.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if there was an argument error.
 */
esp_err_t dmx_get_baud_rate(dmx_port_t dmx_num, uint32_t *baud_rate);

/**
 * @brief Set the DMX break length.
 *
 * @note The break length must be within DMX specification. The lowest value is
 * DMX_TX_MIN_SPACE_FOR_BRK_US. The macro DMX_TX_BRK_DURATION_IS_VALID() may be
 * used to check if a break length is to DMX specification. It should also be
 * noted that there are different timing requirements depending on whether the
 * DMX devices acts as a transmitter or receiver. When checking whether a
 * received break length is valid, use DMX_RX_BRK_DURATION_IS_VALID() instead.
 *
 * @param dmx_num The DMX port number.
 * @param break_num The length in microseconds of the DMX break.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if there was an argument error.
 */
esp_err_t dmx_set_break_len(dmx_port_t dmx_num, uint32_t break_len);

/**
 * @brief Get the DMX break length.
 *
 * @param dmx_num The DMX port number.
 * @param[out] break_num The length in microseconds of the DMX break.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if there was an argument error.
 */
esp_err_t dmx_get_break_len(dmx_port_t dmx_num, uint32_t *break_len);

/**
 * @brief Set the DMX mark-after-break length.
 *
 * @note The mark-after-break length must be within DMX specification. The
 * lowest value is DMX_TX_MIN_MRK_AFTER_BRK_US and the highest value is
 * DMX_TX_MAX_MRK_AFTER_BRK_US (inclusive). The macro
 * DMX_TX_MAB_DURATION_IS_VALID() may be used to check if a mark-after-break
 * length is to DMX specification. It should also be noted that there are
 * different timing requirements depending on whether the DMX
 * devices acts as a transmitter or receiver. When checking whether a received
 * mark-after-break length is valid, use DMX_RX_MAB_DURATION_IS_VALID() instead.
 *
 * @param dmx_num The DMX port number.
 * @param break_num The length in microseconds of the DMX mark-after-break.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if there was an argument error.
 */
esp_err_t dmx_set_mab_len(dmx_port_t dmx_num, uint32_t mab_len);

/**
 * @brief Get the DMX mark-after-break length.
 *
 * @param dmx_num The DMX port number.
 * @param[out] break_num The length in microseconds of the DMX mark-after-break.
 * @retval ESP_OK on success.
 * @retval ESP_ERR_INVALID_ARG if there was an argument error.
 */
esp_err_t dmx_get_mab_len(dmx_port_t dmx_num, uint32_t *mab_len);

/// Read/Write  ###############################################################
size_t dmx_read(dmx_port_t dmx_num, void *data, size_t size);

size_t dmx_write(dmx_port_t dmx_num, const void *data, size_t size);

// TODO: docs
size_t dmx_receive(dmx_port_t dmx_num, dmx_event_t *event,
                 TickType_t ticks_to_wait);

// TODO: docs
size_t dmx_send(dmx_port_t dmx_num, size_t size, TickType_t ticks_to_wait);

// TODO: docs
bool dmx_wait_sent(dmx_port_t dmx_num, TickType_t ticks_to_wait);


void *memcpyswap(void *dest, const void *src, size_t n);

#ifdef __cplusplus
}
#endif
