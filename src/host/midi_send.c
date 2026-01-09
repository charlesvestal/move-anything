#include "midi_send.h"

int host_module_send_midi_bytes(module_manager_t *mm, const uint8_t *msg, int len, int source) {
    if (!mm || !msg || len < 1) return -1;
    mm_on_midi(mm, msg, len, source);
    return 0;
}
