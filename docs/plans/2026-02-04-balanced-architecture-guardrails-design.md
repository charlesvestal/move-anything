# Balanced Architecture Guardrails Design (2026-02-04)

## Context
Move Everything aims to serve three audiences equally: musicians (stability and UX), developers (API stability and tooling), and hackers (low-level power and openness). The current layered architecture is strong for extensibility but fragile at the platform boundary and lacks clear safety and compatibility guardrails.

## Problem Statement
The platform is powerful but brittle:
- System-level hooks and binary replacement are risky for non-hackers.
- A single module can destabilize audio/UI.
- Compatibility rules are not formalized, creating maintenance risk for module authors.

## Goals
- Preserve the existing modular architecture and ecosystem potential.
- Add recovery paths and fault isolation that keep musicians productive.
- Formalize module compatibility so developers can maintain modules confidently.
- Keep advanced capabilities available for hackers with explicit opt-in.

## Non-Goals
- Full container-style sandboxing for all modules.
- Replacing the current shim/host/module architecture.
- Becoming an official Ableton-supported distribution.

## Current Architecture Summary
Layered approach: install bootstrap -> LD_PRELOAD shim -> host runtime -> drop-in modules. UI runs in QuickJS, DSP runs natively. Modules are discovered via `module.json` and optionally distributed through the Module Store.

## Proposed Guardrails
### 1) Safe-Mode and Rollback (Bootstrap Layer)
- Add a boot-time safe-mode path that bypasses Move Everything and starts stock Move.
- Provide a physical escape sequence (e.g., hold a key combo during boot).
- Add a UI action to restore the original binary from backup and reboot.

### 2) Module Health and Containment (Host Runtime)
- Wrap module `init()` and `tick()` calls with crash handling. On error, unload module and return to menu.
- Track module state (Running, Crashed, Disabled) and display status in the UI.
- Persist a disable list so repeated crashes do not block boot.
- Optional Phase 3: isolate DSP or UI in a separate process for true fail-soft behavior.

### 3) Compatibility Contract (Module Loading + Store)
- Formalize `api_version` and `min_host_version` enforcement with user-visible warnings.
- Publish a compatibility policy (e.g., host supports N minor versions of the API).
- Store UI should gate incompatible modules and show clear reasons.

### 4) Capability-Based Safety (Module Metadata)
- Introduce `unsafe_capabilities` (e.g., raw_midi, raw_ui, direct hardware access).
- Require explicit user opt-in for unsafe modules; default to safe modules in the store.

## UX Flows
- Safe-mode boot: hardware combo -> safe mode -> option to disable modules or revert.
- Crash flow: module error -> auto-unload -> toast/status -> module marked disabled.
- Compatibility flow: store warns and blocks incompatible modules; load-time warnings for manual installs.

## Phased Roadmap
Phase 1 (Safety Baseline)
- Safe-mode boot path and rollback UI.
- Module crash handling and disable list.

Phase 2 (Developer Confidence)
- Compatibility policy and enforcement in store and loader.
- Module status UI and logging improvements.
- Unsafe capability labeling and opt-in settings.

Phase 3 (Stronger Isolation)
- Optional process separation for UI or DSP.
- Watchdog restart for crashed modules.

## Success Metrics
- Safe-mode usage rate and successful recovery rate.
- Reduced support incidents tied to module crashes.
- Module install success and update adoption rate.
- Fewer compatibility-related regressions reported by module authors.

## Risks
- Added guardrails may feel restrictive to hackers; mitigate with opt-in unsafe mode.
- False positives in crash detection could disable stable modules.
- Isolation work may increase CPU/memory usage.

## Open Questions
- Which hardware combo is most discoverable and least disruptive for safe-mode?
- What compatibility window (N minor versions) is acceptable?
- Is lightweight telemetry acceptable, or should all metrics be opt-in/offline?
