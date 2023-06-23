#pragma once

#include <stdint.h>

#include "dmx/types.h"
#include "rdm/agent.h"
#include "rdm/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback function type for use with rdm_register_identify_device().
 *
 * @param dmx_num The DMX port number.
 * @param identify The state to which the identify mode was set.
 * @param[inout] context A pointer to a user context.
 */
typedef void (*rdm_identify_cb_t)(dmx_port_t dmx_num, bool identify,
                                  void *context);

/**
 * @brief Registers the default response to RDM_PID_DISC_UNIQUE_BRANCH requests.
 * This response is required by all RDM-capable devices. It is called when the
 * DMX driver is initially installed.
 *
 * @param dmx_num The DMX port number.
 * @return true if the PID response was registered.
 * @return false if there is not enough memory to register additional responses.
 */
bool rdm_register_disc_unique_branch(dmx_port_t dmx_num);

/**
 * @brief Registers the default response to RDM_PID_DISC_MUTE requests. This
 * response is required by all RDM-capable devices. It is called when the DMX
 * driver is initially installed.
 *
 * @param dmx_num The DMX port number.
 * @return true if the PID response was registered.
 * @return false if there is not enough memory to register additional responses.
 */
bool rdm_register_disc_mute(dmx_port_t dmx_num);

/**
 * @brief Registers the default response to RDM_PID_DISC_UN_MUTE requests. This
 * response is required by all RDM-capable devices. It is called when the DMX
 * driver is initially installed.
 *
 * @param dmx_num The DMX port number.
 * @return true if the PID response was registered.
 * @return false if there is not enough memory to register additional responses.
 */
bool rdm_register_disc_un_mute(dmx_port_t dmx_num);

/**
 * @brief Registers the default response to RDM_PID_DEVICE_INFO requests. This
 * response is required by all RDM-capable devices. It is called when the DMX
 * driver is initially installed.
 *
 * @param dmx_num The DMX port number.
 * @param device_info A pointer to the device info parameter to use in RDM
 * responses.
 * @return true if the PID response was registered.
 * @return false if there is not enough memory to register additional responses.
 */
bool rdm_register_device_info(dmx_port_t dmx_num,
                              rdm_device_info_t *device_info);

/**
 * @brief Registers the default response to RDM_PID_SOFTWARE_VERSION_LABEL
 * requests. This response is required by all RDM-capable devices. It is called
 * when the DMX driver is initially installed.
 *
 * @param dmx_num The DMX port number.
 * @param software_version_label A pointer to a null-terminated software version
 * label string to use in RDM responses.
 * @return true if the PID response was registered.
 * @return false if there is not enough memory to register additional responses.
 */
bool rdm_register_software_version_label(dmx_port_t dmx_num,
                                         const char *software_version_label);

/**
 * @brief Registers the default response to RDM_PID_IDENTIFY_DEVICE requests.
 * This response is required by all RDM-capable devices. It is called when the
 * DMX driver is initially installed.
 *
 * @param dmx_num The DMX port number.
 * @param cb A callback function which is called when this device's identify
 * mode state changes.
 * @param[inout] context Context which is passed to the callback function when
 * this device's identify mode state changes.
 * @return true if the PID response was registered.
 * @return false if there is not enough memory to register additional responses.
 */
bool rdm_register_identify_device(dmx_port_t dmx_num, rdm_identify_cb_t cb,
                                  void *context);

/**
 * @brief Registers the default response to RDM_PID_DMX_START_ADDRESS requests.
 * This response is required by all RDM-capable devices which use a DMX address.
 * It is called when the DMX driver is initially installed.
 *
 * @param dmx_num The DMX port number.
 * @param dmx_start_address A pointer to the DMX start address to use in RDM
 * responses.
 * @return true if the PID response was registered.
 * @return false if there is not enough memory to register additional responses.
 */
bool rdm_register_dmx_start_address(dmx_port_t dmx_num,
                                    uint16_t *dmx_start_address);

#ifdef __cplusplus
}
#endif
