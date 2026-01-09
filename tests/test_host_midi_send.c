#include <assert.h>
#include <stdint.h>
#include "src/host/module_manager.h"
#include "src/host/midi_send.h"

static int last_len = -1;
static int last_source = -1;

void mm_on_midi(module_manager_t *mm, const uint8_t *msg, int len, int source) {
    (void)mm;
    (void)msg;
    last_len = len;
    last_source = source;
}

int main(void) {
    module_manager_t mm = {0};
    uint8_t msg[6] = {0xF0, 0x41, 0x10, 0x16, 0x12, 0xF7};

    int rc = host_module_send_midi_bytes(&mm, msg, 6, 2);
    assert(rc == 0);
    assert(last_len == 6);
    assert(last_source == 2);
    return 0;
}
