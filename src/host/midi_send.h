#ifndef MIDI_SEND_H
#define MIDI_SEND_H

#include <stdint.h>
#include "module_manager.h"

int host_module_send_midi_bytes(module_manager_t *mm, const uint8_t *msg, int len, int source);

#endif
