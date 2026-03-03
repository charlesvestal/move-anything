# Current Shape

Live technical reference for external contributors and their AI tools. Maintained automatically during development. Describes how the system works now, including runtime behavior, data contracts, and update-sensitive assumptions.

**Maintenance:** Update sections in-place to reflect current reality. Replace outdated content — never remove a section entirely. Keep the section schema stable, but add new top-level sections when genuinely needed. Every update must add one entry to `## Shifts` with a `ref:` token, and newest entries go at the top. Use `## Notes` for relevant short-lived context. See `CLAUDE.md` for full rules.

## Architecture

- **Host:** C binary (`move_anything.c`) with embedded QuickJS runtime. Manages module lifecycle, MIDI routing, and hardware I/O via `/dev/ablspi0.0`.
- **Shim:** `move_anything_shim.c` — LD_PRELOAD library that intercepts `ioctl` calls to mix shadow audio with stock Move output.
- **Modules:** Self-contained units with `module.json` (metadata), `ui.js` (JS UI), and optional `dsp.so` (native DSP plugin loaded via dlopen).
- **Signal Chain:** The `chain` module loads sub-plugins, routes MIDI through MIDI FX to a sound generator, and pipes audio through audio FX.
- **Shadow Mode:** Runs custom signal chains alongside stock Move firmware. 4 slots, master FX chain, state persistence.
- **Shared Memory IPC:** Shim ↔ host communication via named shared memory segments (`/move-shadow-audio`, `-control`, `-param`, `-ui`).

## Runtime Model

- **Audio model:** 44.1kHz, 128 frames/block, stereo interleaved int16. Host DSP and shim mixing are block-based.
- **Control model:** Hardware MIDI enters mailbox buffers; host JS and DSP callbacks consume events each frame/tick cycle.
- **Execution split:** UI logic runs in QuickJS; real-time DSP runs in native plugins (`dsp.so`); shim handles interception/mixing around stock Move runtime.
- **Failure mode to watch:** If upstream firmware changes mailbox/`ioctl` semantics, shim assumptions can fail even if JS/module code is unchanged.

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

## Data Contracts

- **Module metadata contract:** `module.json` drives discoverability, category routing, capability flags, and some host behavior (for example knob claiming/overtake role).
- **DSP contract:** `move_plugin_init_v2(...)` + v2 function table is expected for modern modules and chain compatibility.
- **Shadow UI contract:** `ui_hierarchy` and `chain_params` are authoritative for editable parameter semantics in Shadow/Chain views.
- **IPC flag contract:** `shadow_control_t.ui_flags` values are behavior switches (jump-to-slot, jump-to-master-FX, jump-to-overtake); changes require sync between shim + UI code.

## Constraints

- **Audio:** 44100 Hz, 128 frames/block, stereo interleaved int16.
- **Display:** 128×64, 1-bit.
- **MIDI buffer overflow:** Output buffer holds ~64 USB-MIDI packets. Overtake modules must use progressive LED init (≤8 LEDs/frame).
- **Overtake init delay:** ~500ms between host LED clear and module `init()`. External USB devices may have already sent handshake requests — send your init proactively, don't wait.
- **MIDI cable filtering:** Cable 0 = internal hardware, Cable 2 = external USB. Normal shadow mode only processes cable 0; overtake mode forwards all cables.
- **MIDI channel echo risk:** If Move tracks listen+output on the same channel as an external device, MIDI loops occur.
- **Ableton firmware dependency:** The shim intercepts stock Move internals. Ableton firmware updates can change memory layouts, ioctl behavior, or audio pipeline details, breaking shim assumptions. Document any firmware-specific adaptations here when they occur.

## Update-Sensitive Areas

- **Shim touchpoints:** `src/move_anything_shim.c`, `src/host/shadow_constants.h`, and any mailbox offset assumptions are highest risk after firmware changes.
- **Routing behavior:** MIDI cable filtering and shadow/overtake routing are fragile integration points and should be revalidated when hardware behavior changes.
- **Chain/Shadow UI compatibility:** Any schema change in `ui_hierarchy`, `chain_params`, or module capability flags can silently break edit/navigation flows.
- **Deployment shape:** On-device paths, category install folders, and Module Store extraction targets must stay aligned with host expectations.

## Firmware Compatibility

- **Track tested versions:** List firmware versions verified with current shim behavior.
- **Record breakages:** Document firmware changes that break mailbox offsets, ioctl expectations, timing assumptions, or MIDI/audio routing.
- **Record mitigations:** For each breakage, record shim-side fix/workaround and whether it is temporary.
- **Use refs:** Include a `ref:` token to commit/PR/issue for each compatibility note.

## Notes

- Use this section for relevant short-lived context that should not overwrite stable sections.
- Keep notes concise and action-oriented; remove or fold into stable sections when no longer needed.

## Shifts

- 2026-03-03: Expanded CURRENT_SHAPE.md with runtime model, data contracts, and update-sensitive areas for downstream AI merge reliability. (ref: N/A)
- 2026-03-03: Initialized CURRENT_SHAPE.md. (ref: N/A)
