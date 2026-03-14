/*
 * Unit tests for AbleM8 SysEx accumulator
 *
 * Run: node tests/modules/ablem8/test_sysex.mjs
 *
 * QuickJS delivers SysEx as 3-byte slices. AbleM8 accumulates fragments
 * until F7 end byte, then processes the complete message. The M8 sends
 * the Universal Device Inquiry (F0 7E 7F 06 01 F7) on connect, and
 * AbleM8 responds by re-sending the LPP init handshake.
 *
 * Tests cover:
 *   1. Single-fragment sysex (F0 start + F7 end in one 3-byte message)
 *   2. Two-fragment sysex: start then end
 *   3. Three-fragment sysex: start, middle, end
 *   4. M8 identity sysex (split) triggers LPP init
 *   5. Non-M8 sysex does NOT trigger LPP init
 *   6. Fragment with no prior F0 is discarded
 *   7. Buffer clears after complete sysex
 *   8. Consecutive sysex messages do not bleed state
 */

import { createHostMock } from '../harness/host_mock.mjs';
import { assertEqual, assertTrue, assertFalse, summarize } from '../harness/assert.mjs';

// ── Helpers ───────────────────────────────────────────────────────────────────

async function freshAbleM8() {
    const url = new URL('../../../src/modules/ablem8/ui.mjs', import.meta.url);
    url.searchParams.set('t', Date.now() + Math.random());
    await import(url.href);
}

// Send an array of MIDI messages to onMidiMessageExternal, simulating
// QuickJS's 3-byte-per-call sysex delivery.
function sendFragments(fragments) {
    for (const f of fragments) {
        globalThis.onMidiMessageExternal(f);
    }
}

// Count how many LPP init packets were sent externally.
// The LPP init is a multi-packet SysEx blob sent as one array via move_midi_external_send.
function countLppInits(mock) {
    // LPP init calls move_midi_external_send with a long packet starting with [0x24, 0xF0, 126, ...]
    return mock.sentExternal.filter(p => p.length > 4 && p[1] === 0xF0 && p[2] === 126).length;
}

// ── Test suites ───────────────────────────────────────────────────────────────

async function testSingleFragmentSysex() {
    console.log('\n--- Single-fragment sysex ---');
    const mock = createHostMock();
    await freshAbleM8();

    mock.clearAll();
    // Sysex that starts and ends in 3 bytes (non-M8, should not trigger init)
    sendFragments([[0xF0, 0x00, 0xF7]]);
    const lppInits = countLppInits(mock);
    assertEqual(lppInits, 0, 'single-fragment non-M8 sysex: no LPP init sent');
}

async function testTwoFragmentSysex() {
    console.log('\n--- Two-fragment sysex ---');
    const mock = createHostMock();
    await freshAbleM8();

    mock.clearAll();
    // First fragment: start
    sendFragments([[0xF0, 0x01, 0x02]]);
    // No external packets sent yet (still accumulating)
    const midInits = countLppInits(mock);
    assertEqual(midInits, 0, 'two-fragment: no init after first fragment');

    // Second fragment: end
    sendFragments([[0x03, 0x04, 0xF7]]);
    // Should have processed (non-M8 sysex, no init)
    assertEqual(countLppInits(mock), 0, 'two-fragment non-M8: no LPP init');
}

async function testThreeFragmentSysex() {
    console.log('\n--- Three-fragment sysex ---');
    const mock = createHostMock();
    await freshAbleM8();

    mock.clearAll();
    sendFragments([
        [0xF0, 0x7E, 0x7F],  // start
        [0x06, 0x02, 0x00],  // middle (note: 0x02 not 0x01 — NOT m8Init)
        [0x00, 0x00, 0xF7],  // end
    ]);
    assertEqual(countLppInits(mock), 0, 'three-fragment non-M8: no LPP init');
}

async function testM8InitTrigger() {
    console.log('\n--- M8 identity sysex triggers LPP init ---');
    const mock = createHostMock();
    await freshAbleM8();

    // M8 identity = [0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7]
    // QuickJS delivers as two 3-byte slices
    mock.clearAll();
    sendFragments([
        [0xF0, 0x7E, 0x7F],
        [0x06, 0x01, 0xF7],
    ]);
    const lppInits = countLppInits(mock);
    assertTrue(lppInits > 0, 'M8 identity sysex: LPP init sent');
}

async function testNonM8SysexNoInit() {
    console.log('\n--- Non-M8 sysex does not trigger LPP init ---');
    const mock = createHostMock();
    await freshAbleM8();

    mock.clearAll();
    // Same length but wrong bytes
    sendFragments([
        [0xF0, 0x7E, 0x7F],
        [0x06, 0x02, 0xF7],  // 0x02 != 0x01
    ]);
    assertEqual(countLppInits(mock), 0, 'wrong sysex: no LPP init');

    mock.clearAll();
    sendFragments([
        [0xF0, 0x41, 0x10],  // Roland manufacturer
        [0x42, 0x12, 0xF7],
    ]);
    assertEqual(countLppInits(mock), 0, 'Roland sysex: no LPP init');
}

async function testOrphanFragmentDiscarded() {
    console.log('\n--- Orphan fragment (no prior F0) discarded ---');
    const mock = createHostMock();
    await freshAbleM8();

    mock.clearAll();
    // Send a fragment ending with F7 without any F0 start
    sendFragments([[0x06, 0x01, 0xF7]]);
    assertEqual(countLppInits(mock), 0, 'orphan fragment: no LPP init, no crash');
}

async function testBufferClearsAfterComplete() {
    console.log('\n--- Buffer clears after complete sysex ---');
    const mock = createHostMock();
    await freshAbleM8();

    // Complete M8 init sysex
    sendFragments([[0xF0, 0x7E, 0x7F], [0x06, 0x01, 0xF7]]);
    mock.clearAll();

    // Now send just an end fragment — should be discarded (buffer was cleared)
    sendFragments([[0x06, 0x01, 0xF7]]);
    assertEqual(countLppInits(mock), 0, 'post-complete orphan F7: no second LPP init');
}

async function testConsecutiveSysexNoBleed() {
    console.log('\n--- Consecutive sysex messages do not bleed ---');
    const mock = createHostMock();
    await freshAbleM8();

    mock.clearAll();
    // First: non-M8 sysex
    sendFragments([[0xF0, 0x00, 0x00], [0x00, 0x00, 0xF7]]);
    assertEqual(countLppInits(mock), 0, 'first sysex (non-M8): no init');

    // Second: M8 identity sysex
    mock.clearAll();
    sendFragments([[0xF0, 0x7E, 0x7F], [0x06, 0x01, 0xF7]]);
    const inits = countLppInits(mock);
    assertTrue(inits > 0, 'second sysex (M8): LPP init sent correctly');
}

// ── Run all suites ────────────────────────────────────────────────────────────

const mock = createHostMock();

await testSingleFragmentSysex();
await testTwoFragmentSysex();
await testThreeFragmentSysex();
await testM8InitTrigger();
await testNonM8SysexNoInit();
await testOrphanFragmentDiscarded();
await testBufferClearsAfterComplete();
await testConsecutiveSysexNoBleed();

mock.enableLog();
summarize('ablem8_sysex');
