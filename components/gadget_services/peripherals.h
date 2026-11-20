#pragma once

#include "gadget_module.h"

void gadget_audio_init(void);
gadget_pcm_status_t gadget_audio_status(void);
void gadget_audio_measure_stack(void);
void gadget_motion_shutdown(void);
void gadget_motion_measure_stack(void);
