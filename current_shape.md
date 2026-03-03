# Current Shape

Live reference for external contributors and their AI tools. Maintained automatically during development. Describes the project as it exists now, not as it was designed or documented elsewhere.

**Maintenance:** Update sections in-place to reflect current reality. Delete stale content. Append to Shifts only for structural changes. See CLAUDE.md for full rules.

## Architecture

- **Host:** C binary (`move_anything.c`) with embedded QuickJS runtime. Manages module lifecycle, MIDI routing, and hardware I/O via `/dev/ablspi0.0`.
- **Shim:** `move_anything_shim.c` — LD_PRELOAD library that intercepts `ioctl` calls to mix shadow audio with stock Move output.
- **Modules:** Self-contained units with `module.json` (metadata), `ui.js` (JS UI), and optional `dsp.so` (native DSP plugin loaded via dlopen).
- **Signal Chain:** The `chain` module loads sub-plugins, routes MIDI through MIDI FX to a sound generator, and pipes audio through audio FX.
- **Shadow Mode:** Runs custom signal chains alongside stock Move firmware. 4 slots, master FX chain, state persistence.
- **Shared Memory IPC:** Shim ↔ host communication via named shared memory segments (`/move-shadow-audio`, `-control`, `-param`, `-ui`).

## Key Files

- `src/move_anything.c` — Main host runtime
- `src/move_anything_shim.c` — LD_PRELOAD shim, hardware interception, audio mixing
- `src/host/plugin_api_v1.h` — DSP plugin C API (v1 deprecated, v2 current)
- `src/host/module_manager.c` — Module discovery and loading
- `src/host/menu_ui.js` — Host menu UI
- `src/host/shadow_constants.h` — Shared memory struct definitions
- `src/shadow/shadow_ui.js` — Shadow mode UI, slot/patch management, overtake lifecycle
- `src/shared/` — JS utilities (constants, MIDI helpers, display, menus, store)
- `src/modules/chain/` — Signal Chain module (chain_host.c, patches)
- `src/modules/controller/` — MIDI Controller (overtake example)
- `src/modules/store/` — Module Store (catalog fetch, install/remove)

## Conventions

- **Plugin API v2** for all new modules. v1 is singleton-only and deprecated.
- **Module categorization** via `component_type` in module.json: `featured`, `sound_generator`, `audio_fx`, `midi_fx`, `utility`, `overtake`, `system`.
- **External modules** install to category subdirectories: `modules/sound_generators/<id>/`, `modules/audio_fx/<id>/`, etc.
- **Chainable modules** must declare `capabilities.chainable: true` and expose `chain_params` + `ui_hierarchy` via `get_param`.
- **Parameter metadata** uses `chain_params` (type, min, max, step, options). **Menu structure** uses `ui_hierarchy` (knobs array, params array with key/label or level/label objects).

## Constraints

- **Audio:** 44100 Hz, 128 frames/block, stereo interleaved int16.
- **Display:** 128×64, 1-bit.
- **MIDI buffer overflow:** Output buffer holds ~64 USB-MIDI packets. Overtake modules must use progressive LED init (≤8 LEDs/frame).
- **Overtake init delay:** ~500ms between host LED clear and module `init()`. External USB devices may have already sent handshake requests — send your init proactively, don't wait.
- **MIDI cable filtering:** Cable 0 = internal hardware, Cable 2 = external USB. Normal shadow mode only processes cable 0; overtake mode forwards all cables.
- **MIDI channel echo risk:** If Move tracks listen+output on the same channel as an external device, MIDI loops occur.

## Shifts

- 2026-03-03: Initialized current_shape.md.
