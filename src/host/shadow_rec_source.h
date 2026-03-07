#ifndef SHADOW_REC_SOURCE_H
#define SHADOW_REC_SOURCE_H

#include "plugin_api_v1.h"
#include <stdint.h>

typedef struct {
    void *dl_handle;                    /* dlopen handle */
    plugin_api_v2_t *api;               /* Plugin API v2 */
    void *instance;                     /* Plugin instance */
    char module_id[64];                 /* Module ID (e.g. "webstream") */
    char module_abbrev[8];              /* Abbreviation (e.g. "WS") */
    char module_name[64];               /* Display name */
    char module_path[256];              /* Path to module directory */
    int active;                         /* Slot is loaded and active */
    float peak_level;                   /* Current audio peak level (0.0-1.0) */
    int16_t audio_buffer[256];          /* 128 frames * 2 channels (stereo int16) */
} shadow_rec_source_t;

/* Global instance */
extern shadow_rec_source_t shadow_rec_source;

/* Load a rec source module by ID. Returns 0 on success. */
int rec_source_load(const char *module_id);

/* Unload the current rec source. */
void rec_source_unload(void);

/* Render one block of audio from the source. Called in render pipeline. */
void rec_source_render(void);

/* Pause/resume the source plugin's audio output. */
void rec_source_pause(void);
void rec_source_resume(void);

/* Returns 1 if source is actively producing audio. */
int rec_source_is_active(void);

/* Returns current peak audio level (0.0-1.0). */
float rec_source_get_level(void);

/* Returns pointer to the raw audio buffer (for sampler tap). */
const int16_t *rec_source_get_audio(void);

#endif /* SHADOW_REC_SOURCE_H */
