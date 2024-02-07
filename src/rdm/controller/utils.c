#include "rdm/controller/include/utils.h"

#include <string.h>

#include "dmx/include/driver.h"
#include "dmx/include/service.h"
#include "rdm/include/driver.h"
#include "rdm/include/uid.h"

size_t rdm_send_request(dmx_port_t dmx_num,
                        const rdm_transaction_t *transaction, void *pd,
                        size_t size, rdm_ack_t *ack) {
  assert(dmx_num < DMX_NUM_MAX);
  assert(transaction != NULL);
  assert(transaction->dest_uid != NULL);
  assert(transaction->sub_device < RDM_SUB_DEVICE_MAX ||
         transaction->sub_device == RDM_SUB_DEVICE_ALL);
  assert(transaction->pid > 0);
  assert(rdm_cc_is_valid(transaction->cc) &&
         rdm_cc_is_request(transaction->cc));
  assert(transaction->sub_device != RDM_SUB_DEVICE_ALL ||
         transaction->cc == RDM_CC_SET_COMMAND);
  assert(rdm_format_is_valid(transaction->format.transaction));
  assert(transaction->format.transaction != NULL || transaction->pd == NULL);
  assert(transaction->pd != NULL || transaction->pdl == 0);
  assert(transaction->pdl < RDM_PD_SIZE_MAX);
  assert(rdm_format_is_valid(transaction->format.transaction));
  assert(dmx_driver_is_installed(dmx_num));

  dmx_driver_t *const driver = dmx_driver[dmx_num];

  // Attempt to take the mutex and wait until the driver is done sending
  if (!xSemaphoreTakeRecursive(driver->mux, portMAX_DELAY)) {
    return 0;
  }
  if (!dmx_wait_sent(dmx_num, dmx_ms_to_ticks(23))) {
    xSemaphoreGiveRecursive(driver->mux);
    return 0;
  }

  // Construct the header using the default arguments and the caller's arguments
  rdm_header_t header = {
      .message_len = 24 + transaction->pdl,
      .tn = rdm_get_transaction_num(dmx_num),
      .port_id = dmx_num + 1,
      .message_count = 0,
      .sub_device = transaction->sub_device,
      .cc = transaction->cc,
      .pid = transaction->pid,
      .pdl = transaction->pdl,
  };
  memcpy(&header.dest_uid, transaction->dest_uid, sizeof(header.dest_uid));
  memcpy(&header.src_uid, rdm_uid_get(dmx_num), sizeof(header.src_uid));

  // Copy the old data in the DMX buffer to a temporary buffer
  uint8_t old_data[257];
  const size_t packet_size = header.message_len + 2; 
  dmx_read(dmx_num, old_data, packet_size);

  // Write and send the RDM request
  rdm_write(dmx_num, &header, transaction->format.transaction, transaction->pd);
  if (!dmx_send(dmx_num)) {
    dmx_write(dmx_num, old_data, packet_size);  // Write old data back
    xSemaphoreGiveRecursive(driver->mux);
    if (ack != NULL) {
      ack->err = DMX_OK;
      ack->size = 0;
      ack->src_uid = (rdm_uid_t){0, 0};
      ack->pid = 0;
      ack->type = RDM_RESPONSE_TYPE_NONE;
      ack->message_count = 0;
      ack->pdl = 0;
    }
    return 0;
  }

  // Return early if no response is expected
  if (rdm_uid_is_broadcast(transaction->dest_uid) &&
      transaction->pid != RDM_PID_DISC_UNIQUE_BRANCH) {
    dmx_wait_sent(dmx_num, dmx_ms_to_ticks(23));
    dmx_write(dmx_num, old_data, packet_size);  // Write old data back
    xSemaphoreGiveRecursive(driver->mux);
    if (ack != NULL) {
      ack->err = DMX_OK;
      ack->size = 0;
      ack->src_uid = (rdm_uid_t){0, 0};
      ack->pid = 0;
      ack->type = RDM_RESPONSE_TYPE_NONE;
      ack->message_count = 0;
      ack->pdl = 0;
    }
    return 0;
  }

  // Attempt to receive the RDM response
  dmx_packet_t packet;
  dmx_receive(dmx_num, &packet, dmx_ms_to_ticks(23));
  if (ack != NULL) {
    ack->err = packet.err;
    ack->size = packet.size;
  }

  // Return early if no response was received
  if (packet.size == 0) {
    dmx_write(dmx_num, old_data, packet_size);  // Write old data back
    xSemaphoreGiveRecursive(driver->mux);
    if (ack != NULL) {
      ack->src_uid = (rdm_uid_t){0, 0};
      ack->pid = 0;
      ack->type = RDM_RESPONSE_TYPE_NONE;
      ack->message_count = 0;
      ack->pdl = 0;
    }
    return 0;
  }

  // Return early if the response checksum was invalid
  if (!rdm_read_header(dmx_num, &header)) {
    dmx_write(dmx_num, old_data, packet_size);  // Write old data back
    xSemaphoreGiveRecursive(driver->mux);
    if (ack != NULL) {
      ack->src_uid = (rdm_uid_t){0, 0};
      ack->pid = 0;
      ack->type = RDM_RESPONSE_TYPE_INVALID;
      ack->message_count = 0;
      ack->pdl = 0;
    }
    return 0;
  }

  // Copy the parameter data into the output
  if (header.response_type == RDM_RESPONSE_TYPE_ACK &&
      header.pid != RDM_PID_DISC_UNIQUE_BRANCH) {
    if (pd == NULL) {
      rdm_read_pd(dmx_num, transaction->format.response, pd, size);
    }
  }

  // Copy the results into the ack struct
  if (ack != NULL) {
    memcpy(&ack->src_uid, &header.src_uid, sizeof(rdm_uid_t));
    ack->pid = header.pid;
    if (!rdm_response_type_is_valid(header.response_type)) {
      ack->type = RDM_RESPONSE_TYPE_INVALID;
      ack->pdl = header.pdl;
    } else {
      const char *word_format = "w";
      ack->type = header.response_type;
      if (header.response_type == RDM_RESPONSE_TYPE_ACK_TIMER) {
        uint16_t timer;
        rdm_read_pd(dmx_num, word_format, &timer, sizeof(timer));
        ack->timer = dmx_ms_to_ticks(timer * 10);
      } else if (header.response_type == RDM_RESPONSE_TYPE_NACK_REASON) {
        uint16_t nack_reason;
        rdm_read_pd(dmx_num, word_format, &nack_reason, sizeof(nack_reason));
        ack->nack_reason = nack_reason;
      } else {
        ack->pdl = header.pdl;
      }
    }
    ack->message_count = header.message_count;
  }

  // Write the old data from before the request back into the DMX driver
  dmx_write(dmx_num, old_data, packet_size);

  // Give the mutex back and return the PDL or true on success
  xSemaphoreGiveRecursive(driver->mux);
  if (header.response_type == RDM_RESPONSE_TYPE_ACK) {
    if (header.pdl == 0) {
      return 1;
    } else {
      return header.pdl;
    }
  } else {

    return 0;
  }
}

uint32_t rdm_get_transaction_num(dmx_port_t dmx_num) {
  assert(dmx_num < DMX_NUM_MAX);
  assert(dmx_driver_is_installed(dmx_num));

  uint32_t tn;
  taskENTER_CRITICAL(DMX_SPINLOCK(dmx_num));
  tn = dmx_driver[dmx_num]->rdm.tn;
  taskEXIT_CRITICAL(DMX_SPINLOCK(dmx_num));

  return tn;
}
