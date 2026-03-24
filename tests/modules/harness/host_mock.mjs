/*
 * Host mock for Move module unit tests.
 *
 * Injects all QuickJS host globals into globalThis so ES modules that call
 * move_midi_external_send(), move_midi_internal_send(), etc. work in Node.js.
 *
 * Usage:
 *   import { createHostMock } from '../harness/host_mock.mjs';
 *   const mock = createHostMock();
 *   // ... import module under test ...
 *   mock.sentExternal  // array of packets sent externally
 *   mock.sentInternal  // array of packets sent internally
 *   mock.clearAll()    // reset captured packets between test cases
 */

export function createHostMock() {
    const sentExternal = [];
    const sentInternal = [];
    const injectedToMove = [];
    const consoleLogs = [];

    globalThis.move_midi_external_send = (packet) => {
        sentExternal.push(Array.from(packet));
    };

    globalThis.move_midi_internal_send = (packet) => {
        sentInternal.push(Array.from(packet));
    };

    globalThis.move_midi_inject_to_move = (packet) => {
        injectedToMove.push(Array.from(packet));
    };

    globalThis.clear_screen = () => {};
    globalThis.print = () => {};

    // Silence module console.log output during tests (noisy debug logs).
    // Call mock.enableLog() to restore if you need to debug.
    const origLog = console.log;
    const silencedLog = (...args) => { consoleLogs.push(args.join(' ')); };
    globalThis.console = { log: silencedLog };

    const mock = {
        sentExternal,
        sentInternal,
        injectedToMove,
        consoleLogs,

        clearAll() {
            sentExternal.length = 0;
            sentInternal.length = 0;
            injectedToMove.length = 0;
            consoleLogs.length = 0;
        },

        findExternal(pred) { return sentExternal.filter(pred); },
        findInternal(pred) { return sentInternal.filter(pred); },

        // Restore console.log (e.g. before calling summarize() or for debugging)
        enableLog() {
            globalThis.console = { log: origLog };
        }
    };

    return mock;
}
