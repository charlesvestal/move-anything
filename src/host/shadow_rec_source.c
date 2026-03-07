/*
 * Shadow Rec Source - standalone audio slot for recording from source plugins
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>

#include "shadow_rec_source.h"
#include "shadow_chain_mgmt.h"  /* for shadow_host_api, shadow_log */

#define REC_SOURCE_MODULES_DIR "/data/UserData/move-anything/modules"

/* Category subdirectories to scan for modules */
static const char *category_subdirs[] = {
    "sound_generators",
    "audio_fx",
    "midi_fx",
    "utilities",
    "other",
    "overtake",
    "tools",
    NULL
};

/* Global instance */
shadow_rec_source_t shadow_rec_source;

/* ============================================================================
 * Simple JSON helpers (match module_manager.c style)
 * ============================================================================ */

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\"", key);

    const char *pos = strstr(json, search);
    if (!pos) return -1;

    pos = strchr(pos + strlen(search), ':');
    if (!pos) return -1;

    /* Skip whitespace */
    while (*pos && (*pos == ':' || *pos == ' ' || *pos == '\t' || *pos == '\n')) pos++;

    if (*pos != '"') return -1;
    pos++; /* skip opening quote */

    int i = 0;
    while (*pos && *pos != '"' && i < out_len - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return i;
}

/* ============================================================================
 * Module scanning - find a module by ID
 * ============================================================================ */

/*
 * Try to find a module directory containing module.json with matching id.
 * On success, fills module_dir (path to module directory), name, abbrev, dsp_file.
 * Returns 0 on success, -1 if not found.
 */
static int find_module_by_id(const char *module_id,
                             char *module_dir, int module_dir_len,
                             char *name, int name_len,
                             char *abbrev, int abbrev_len,
                             char *dsp_file, int dsp_file_len) {
    /* Directories to scan: base dir + each category subdir */
    char scan_dirs[16][512];
    int num_dirs = 0;

    snprintf(scan_dirs[num_dirs++], sizeof(scan_dirs[0]), "%s", REC_SOURCE_MODULES_DIR);
    for (int i = 0; category_subdirs[i] && num_dirs < 16; i++) {
        snprintf(scan_dirs[num_dirs++], sizeof(scan_dirs[0]), "%s/%s",
                 REC_SOURCE_MODULES_DIR, category_subdirs[i]);
    }

    for (int d = 0; d < num_dirs; d++) {
        DIR *dir = opendir(scan_dirs[d]);
        if (!dir) continue;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            if (strstr(entry->d_name, "..") != NULL || strchr(entry->d_name, '/') != NULL) continue;

            char path[512];
            snprintf(path, sizeof(path), "%s/%s", scan_dirs[d], entry->d_name);

            struct stat st;
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

            /* Check for module.json */
            char json_path[512];
            snprintf(json_path, sizeof(json_path), "%s/module.json", path);
            if (stat(json_path, &st) != 0) continue;

            /* Read and parse module.json */
            FILE *f = fopen(json_path, "r");
            if (!f) continue;

            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);

            if (len < 0 || len > 8192) {
                fclose(f);
                continue;
            }

            char *json = malloc(len + 1);
            if (!json) {
                fclose(f);
                continue;
            }

            size_t read_len = fread(json, 1, len, f);
            json[read_len] = '\0';
            fclose(f);

            char id[64] = {0};
            json_get_string(json, "id", id, sizeof(id));

            if (strcmp(id, module_id) == 0) {
                /* Found it */
                strncpy(module_dir, path, module_dir_len - 1);
                module_dir[module_dir_len - 1] = '\0';

                if (json_get_string(json, "name", name, name_len) < 0) {
                    strncpy(name, module_id, name_len - 1);
                    name[name_len - 1] = '\0';
                }

                if (json_get_string(json, "abbrev", abbrev, abbrev_len) < 0) {
                    /* Default abbreviation: first two chars of id, uppercased */
                    abbrev[0] = (module_id[0] >= 'a' && module_id[0] <= 'z')
                                ? module_id[0] - 32 : module_id[0];
                    abbrev[1] = (module_id[1] >= 'a' && module_id[1] <= 'z')
                                ? module_id[1] - 32 : module_id[1];
                    abbrev[2] = '\0';
                }

                if (json_get_string(json, "dsp", dsp_file, dsp_file_len) < 0) {
                    strncpy(dsp_file, "dsp.so", dsp_file_len - 1);
                    dsp_file[dsp_file_len - 1] = '\0';
                }

                free(json);
                closedir(dir);
                return 0;
            }

            free(json);
        }

        closedir(dir);
    }

    return -1;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

int rec_source_load(const char *module_id) {
    char msg[256];

    if (!module_id || !module_id[0]) {
        fprintf(stderr, "rec_source_load: empty module_id\n");
        return -1;
    }

    /* Unload existing source if loaded */
    if (shadow_rec_source.active) {
        rec_source_unload();
    }

    /* Find the module */
    char module_dir[256] = {0};
    char name[64] = {0};
    char abbrev[8] = {0};
    char dsp_file[128] = {0};

    if (find_module_by_id(module_id, module_dir, sizeof(module_dir),
                          name, sizeof(name), abbrev, sizeof(abbrev),
                          dsp_file, sizeof(dsp_file)) != 0) {
        snprintf(msg, sizeof(msg), "rec_source_load: module '%s' not found", module_id);
        shadow_log(msg);
        return -1;
    }

    /* Construct DSP path */
    char dsp_path[512];
    snprintf(dsp_path, sizeof(dsp_path), "%s/%s", module_dir, dsp_file);

    snprintf(msg, sizeof(msg), "rec_source_load: loading %s from %s", module_id, dsp_path);
    shadow_log(msg);

    /* dlopen the DSP */
    void *dl = dlopen(dsp_path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) {
        snprintf(msg, sizeof(msg), "rec_source_load: dlopen failed: %s", dlerror());
        shadow_log(msg);
        return -1;
    }

    /* Look for v2 init symbol */
    move_plugin_init_v2_fn init_v2 = (move_plugin_init_v2_fn)dlsym(dl, MOVE_PLUGIN_INIT_V2_SYMBOL);
    if (!init_v2) {
        snprintf(msg, sizeof(msg), "rec_source_load: %s not found in %s",
                 MOVE_PLUGIN_INIT_V2_SYMBOL, dsp_path);
        shadow_log(msg);
        dlclose(dl);
        return -1;
    }

    /* Initialize plugin with host API */
    plugin_api_v2_t *api = init_v2(&shadow_host_api);
    if (!api || !api->create_instance) {
        snprintf(msg, sizeof(msg), "rec_source_load: v2 init failed for %s", module_id);
        shadow_log(msg);
        dlclose(dl);
        return -1;
    }

    /* Create instance */
    void *instance = api->create_instance(module_dir, NULL);
    if (!instance) {
        snprintf(msg, sizeof(msg), "rec_source_load: create_instance failed for %s", module_id);
        shadow_log(msg);
        dlclose(dl);
        return -1;
    }

    /* Tell the plugin it's in rec source mode */
    if (api->set_param) {
        api->set_param(instance, "rec_source_mode", "1");
    }

    /* Populate the slot */
    memset(&shadow_rec_source, 0, sizeof(shadow_rec_source));
    shadow_rec_source.dl_handle = dl;
    shadow_rec_source.api = api;
    shadow_rec_source.instance = instance;
    strncpy(shadow_rec_source.module_id, module_id, sizeof(shadow_rec_source.module_id) - 1);
    strncpy(shadow_rec_source.module_name, name, sizeof(shadow_rec_source.module_name) - 1);
    strncpy(shadow_rec_source.module_abbrev, abbrev, sizeof(shadow_rec_source.module_abbrev) - 1);
    strncpy(shadow_rec_source.module_path, module_dir, sizeof(shadow_rec_source.module_path) - 1);
    shadow_rec_source.active = 1;

    snprintf(msg, sizeof(msg), "rec_source_load: loaded '%s' (%s) [%s]", name, module_id, abbrev);
    shadow_log(msg);

    return 0;
}

void rec_source_unload(void) {
    if (!shadow_rec_source.active) return;

    char msg[128];
    snprintf(msg, sizeof(msg), "rec_source_unload: unloading '%s'", shadow_rec_source.module_id);
    shadow_log(msg);

    /* Destroy instance */
    if (shadow_rec_source.api && shadow_rec_source.api->destroy_instance &&
        shadow_rec_source.instance) {
        shadow_rec_source.api->destroy_instance(shadow_rec_source.instance);
    }

    /* Close shared library */
    if (shadow_rec_source.dl_handle) {
        dlclose(shadow_rec_source.dl_handle);
    }

    /* Zero everything */
    memset(&shadow_rec_source, 0, sizeof(shadow_rec_source));
}

/* ============================================================================
 * Audio rendering
 * ============================================================================ */

void rec_source_render(void) {
    if (!shadow_rec_source.active || !shadow_rec_source.instance ||
        !shadow_rec_source.api || !shadow_rec_source.api->render_block) {
        memset(shadow_rec_source.audio_buffer, 0, sizeof(shadow_rec_source.audio_buffer));
        shadow_rec_source.peak_level = 0.0f;
        return;
    }

    shadow_rec_source.api->render_block(shadow_rec_source.instance,
                                         shadow_rec_source.audio_buffer,
                                         MOVE_FRAMES_PER_BLOCK);

    /* Compute peak level */
    int max_abs = 0;
    for (int i = 0; i < MOVE_FRAMES_PER_BLOCK * 2; i++) {
        int val = shadow_rec_source.audio_buffer[i];
        if (val < 0) val = -val;
        if (val > max_abs) max_abs = val;
    }
    shadow_rec_source.peak_level = (float)max_abs / 32768.0f;
}

/* ============================================================================
 * Transport control
 * ============================================================================ */

void rec_source_pause(void) {
    if (!shadow_rec_source.active || !shadow_rec_source.instance ||
        !shadow_rec_source.api || !shadow_rec_source.api->set_param) {
        return;
    }
    shadow_rec_source.api->set_param(shadow_rec_source.instance, "pause", "1");
}

void rec_source_resume(void) {
    if (!shadow_rec_source.active || !shadow_rec_source.instance ||
        !shadow_rec_source.api || !shadow_rec_source.api->set_param) {
        return;
    }
    shadow_rec_source.api->set_param(shadow_rec_source.instance, "resume", "1");
}

/* ============================================================================
 * Status queries
 * ============================================================================ */

int rec_source_is_active(void) {
    if (!shadow_rec_source.active || !shadow_rec_source.instance ||
        !shadow_rec_source.api || !shadow_rec_source.api->get_param) {
        return 0;
    }

    char buf[16] = {0};
    shadow_rec_source.api->get_param(shadow_rec_source.instance,
                                      "playback_active", buf, sizeof(buf));
    return (strcmp(buf, "1") == 0) ? 1 : 0;
}

float rec_source_get_level(void) {
    return shadow_rec_source.peak_level;
}

const int16_t *rec_source_get_audio(void) {
    return shadow_rec_source.audio_buffer;
}
