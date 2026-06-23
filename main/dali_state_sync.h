#pragma once

#include "dali_protocol.h"
#include "dali_sniffer.h"

void dali_state_sync_handle_frame(const dali_frame_event_t &frame, const dali_frame_description_t &description);
