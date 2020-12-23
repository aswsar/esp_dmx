#pragma once

#include "hal/uart_types.h"
#include "soc/uart_periph.h"

/**
 * @brief Gets the number of bits the UART remains idle after transmitting data.
 * 
 * @param hw Pointer to a UART struct.
 * @return The number of bits the UART is idle after transmitting data. 
 */
inline uint16_t dmx_ll_get_idle_num(uart_dev_t *hw);

/**
 * @brief Gets the number of bits the UART sends as break.
 * 
 * @param hw Pointer to a UART struct.
 * @return The number of bits the UART sends as a break after transmitting.
 */
inline uint8_t dmx_ll_get_break_num(uart_dev_t *hw);