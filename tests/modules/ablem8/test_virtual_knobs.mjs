/*
 * Unit tests for src/shared/move_virtual_knobs.mjs
 *
 * Run: node tests/modules/ablem8/test_virtual_knobs.mjs
 *
 * Tests cover:
 *   1. Bank management (setPotBank, setPotTrack, setTrackScopedPots, setPotShiftHeld)
 *   2. configurePotBank overrides and state reset
 *   3. M8_TRACK bank routing (CC base 71, internal echo, relative encoding)
 *   4. M8_MASTER bank routing (CC base 71, no per-track state)
 *   5. M8_FX bank routing (CC base 90)
 *   6. ABLETON bank routing (volume/sendA/master/return, shift layer, encoder modes)
 *   7. Out-of-range CC rejection
 *   8. Track-scoped pots
 */

import { createHostMock } from '../harness/host_mock.mjs';
import { assertEqual, assertNotEqual, assertArrayEqual, assertTrue, assertFalse, summarize, resetCounts } from '../harness/assert.mjs';

// ── Helpers ──────────────────────────────────────────────────────────────────

// Load a fresh module instance (bypasses Node module cache via query param)
async function freshKnobs() {
    const url = new URL('../../../src/shared/move_virtual_knobs.mjs', import.meta.url);
    url.searchParams.set('t', Date.now() + Math.random());
    return await import(url.href);
}

// Build a CC message [0xb0, cc, value]
function ccMsg(cc, value) {
    return [0xb0, cc, value];
}

// ── Test suites ───────────────────────────────────────────────────────────────

async function testBankManagement() {
    console.log('\n--- Bank management ---');
    const mock = createHostMock();
    const { setPotBank, setPotTrack, setTrackScopedPots, setPotShiftHeld, POT_BANKS, handleMoveKnobs } = await freshKnobs();

    // Unknown bank is a no-op — default bank stays M8_TRACK
    setPotBank('UNKNOWN_BANK');
    mock.clearAll();
    handleMoveKnobs(ccMsg(71, 1)); // CC 71 = first knob in M8_TRACK
    assertTrue(mock.sentExternal.length > 0, 'unknown bank no-op: still routes to default M8_TRACK');

    // setPotTrack clamps 0–7
    setPotTrack(3);
    mock.clearAll();
    setPotTrack(-1); // should clamp to 0
    setPotTrack(9);  // should clamp to 7
    // No assertion on internal state — observable through track-scoped routing
    assertEqual(1, 1, 'setPotTrack clamp does not throw');

    // setTrackScopedPots and setPotShiftHeld are no-throws
    setTrackScopedPots(true);
    setTrackScopedPots(false);
    setPotShiftHeld(true);
    setPotShiftHeld(false);
    assertEqual(1, 1, 'modifier setters do not throw');
}

async function testConfigurePotBank() {
    console.log('\n--- configurePotBank ---');
    const mock = createHostMock();
    const { configurePotBank, handleMoveKnobs, POT_BANKS } = await freshKnobs();

    // Override ABLETON channel to 9, then verify the outgoing packet channel
    configurePotBank(POT_BANKS.ABLETON, { channel: 9 });
    mock.clearAll();
    handleMoveKnobs(ccMsg(71, 10), { activeBank: POT_BANKS.ABLETON });
    assertTrue(mock.sentExternal.length > 0, 'ABLETON routes after channel override');
    const pkt = mock.sentExternal[0];
    // packet[1] is status byte: 0xb0 | channel
    assertEqual(pkt[1] & 0x0f, 9, 'ABLETON overridden channel is 9');

    // configurePotBank on unknown bank is a no-op
    configurePotBank('FAKE', { channel: 5 });
    assertEqual(1, 1, 'configurePotBank unknown bank no-op does not throw');

    // After configure, old accumulated values are reset
    const { configurePotBank: cfg2, handleMoveKnobs: hk2, POT_BANKS: PB2 } = await freshKnobs();
    // Accumulate a value
    mock.clearAll();
    hk2(ccMsg(71, 1)); // +1
    hk2(ccMsg(71, 1)); // +2
    // Now reconfigure — should reset state arrays
    cfg2(PB2.M8_TRACK, { potCount: 10 });
    mock.clearAll();
    hk2(ccMsg(71, 1)); // value should be 1 again after reset
    assertEqual(mock.sentExternal[0][3], 1, 'state reset after configurePotBank');
}

async function testM8TrackBank() {
    console.log('\n--- M8_TRACK bank ---');
    const mock = createHostMock();
    const { handleMoveKnobs, POT_BANKS } = await freshKnobs();

    // CC 71 = potIndex 0, expect external packet with cable 2 and channel 3
    mock.clearAll();
    const result = handleMoveKnobs(ccMsg(71, 1));
    assertTrue(result && result.handled, 'M8_TRACK: handled flag set');
    assertEqual(result.bank, POT_BANKS.M8_TRACK, 'M8_TRACK: bank in result');
    assertEqual(result.potIndex, 0, 'M8_TRACK: potIndex 0');

    // External packet format: [cable<<4|0xb, 0xb0|channel, cc, value]
    const ext = mock.sentExternal[0];
    assertEqual(ext[0] & 0xF0, 0x20, 'M8_TRACK external: cable 2');
    assertEqual(ext[1] & 0x0f, 3,   'M8_TRACK external: channel 3');
    assertEqual(ext[2], 71,          'M8_TRACK external: CC 71');
    assertEqual(ext[3], 1,           'M8_TRACK external: value 1');

    // Internal echo present for M8_TRACK
    assertTrue(mock.sentInternal.length > 0, 'M8_TRACK: internal echo present');
    const intPkt = mock.sentInternal[0];
    assertEqual(intPkt[0] & 0xF0, 0x00, 'M8_TRACK internal: cable 0');

    // Relative encoding: two +1 increments → value 2
    mock.clearAll();
    handleMoveKnobs(ccMsg(71, 1)); // +1 → 1 (already at 1 from above, becomes 2)
    // Note: module state carries over; this second call is +1 from 1 → 2
    handleMoveKnobs(ccMsg(71, 1));
    // The sent values should be accumulating
    assertTrue(mock.sentExternal.length === 2, 'M8_TRACK: two relative increments produce two packets');
    assertEqual(mock.sentExternal[0][3], 2, 'M8_TRACK: first increment → 2');
    assertEqual(mock.sentExternal[1][3], 3, 'M8_TRACK: second increment → 3');

    // Value clamps at 127
    const { handleMoveKnobs: hk2 } = await freshKnobs();
    mock.clearAll();
    for (let i = 0; i < 130; i++) hk2(ccMsg(71, 1)); // add 130 to 0
    const lastPkt = mock.sentExternal[mock.sentExternal.length - 1];
    assertEqual(lastPkt[3], 127, 'M8_TRACK: value clamped at 127');

    // CC 80 = potIndex 9 (last valid), CC 81 = out of range
    mock.clearAll();
    const r80 = handleMoveKnobs(ccMsg(80, 10));
    assertTrue(r80 && r80.handled, 'M8_TRACK: CC 80 = potIndex 9 handled');
    const r81 = handleMoveKnobs(ccMsg(81, 10));
    assertFalse(r81, 'M8_TRACK: CC 81 (out of range) returns false');

    // Non-CC message rejected
    const rNote = handleMoveKnobs([0x90, 71, 100]);
    assertFalse(rNote, 'M8_TRACK: non-CC message returns false');
}

async function testM8MasterBank() {
    console.log('\n--- M8_MASTER bank ---');
    const mock = createHostMock();
    const { handleMoveKnobs, setPotBank, POT_BANKS } = await freshKnobs();

    setPotBank(POT_BANKS.M8_MASTER);
    mock.clearAll();

    // CC 71 still maps to M8_MASTER (ccBase = 71)
    const result = handleMoveKnobs(ccMsg(71, 5));
    assertTrue(result && result.handled, 'M8_MASTER: handled');
    assertEqual(result.bank, POT_BANKS.M8_MASTER, 'M8_MASTER: bank in result');

    // Internal echo present
    assertTrue(mock.sentInternal.length > 0, 'M8_MASTER: internal echo present');

    // M8_FX CC 71 should be out of range for M8_MASTER when using M8_FX bank
    const { handleMoveKnobs: hk2, setPotBank: spb2, POT_BANKS: PB2 } = await freshKnobs();
    spb2(PB2.M8_FX);
    mock.clearAll();
    const rFx = hk2(ccMsg(71, 5)); // M8_FX ccBase = 90, so 71 is out of range
    assertFalse(rFx, 'M8_FX: CC 71 out of range (ccBase is 90)');
    const rFx90 = hk2(ccMsg(90, 5)); // CC 90 = potIndex 0 in M8_FX
    assertTrue(rFx90 && rFx90.handled, 'M8_FX: CC 90 = potIndex 0 handled');
    assertEqual(rFx90.bank, PB2.M8_FX, 'M8_FX: bank in result');
}

async function testAbletonBankRouting() {
    console.log('\n--- ABLETON bank routing ---');
    const mock = createHostMock();
    const { handleMoveKnobs, setPotBank, setPotShiftHeld, POT_BANKS } = await freshKnobs();

    setPotBank(POT_BANKS.ABLETON);

    // Knob 0 (CC 71) with shift NOT held → volumeCcs[0] = CC 0, channel 15
    mock.clearAll();
    const r = handleMoveKnobs(ccMsg(71, 10), { activeBank: POT_BANKS.ABLETON });
    assertTrue(r && r.handled, 'ABLETON: knob 0 handled');
    assertEqual(r.cc, 0, 'ABLETON: knob 0 routes to volumeCcs[0] = CC 0');
    assertEqual(mock.sentExternal[0][1] & 0x0f, 15, 'ABLETON: channel 15');
    // No internal echo
    assertEqual(mock.sentInternal.length, 0, 'ABLETON: no internal echo');

    // Knob 0 with shift held → sendACcs[0] = CC 16
    mock.clearAll();
    setPotShiftHeld(true);
    const rShift = handleMoveKnobs(ccMsg(71, 10), { activeBank: POT_BANKS.ABLETON });
    assertEqual(rShift.cc, 16, 'ABLETON: shift held → sendACcs[0] = CC 16');
    setPotShiftHeld(false);

    // Knob 9 = masterIndex (potCount-1 = 9), CC = ccBase + 9 = 71 + 9 = 80
    // Without shift → masterCc = CC 127
    mock.clearAll();
    const rMaster = handleMoveKnobs(ccMsg(80, 10), { activeBank: POT_BANKS.ABLETON });
    assertEqual(rMaster.cc, 127, 'ABLETON: knob 9 = masterCc 127');

    // With shift → returnCc = CC 117
    mock.clearAll();
    const rReturn = handleMoveKnobs(ccMsg(80, 10), { activeBank: POT_BANKS.ABLETON, shiftHeld: true });
    assertEqual(rReturn.cc, 117, 'ABLETON: knob 9 shift = returnCc 117');
}

async function testAbletonEncoderModes() {
    console.log('\n--- ABLETON encoder modes ---');
    const mock = createHostMock();
    const { handleMoveKnobs, configurePotBank, POT_BANKS } = await freshKnobs();

    // Absolute mode (default): accumulated value sent outgoing
    mock.clearAll();
    handleMoveKnobs(ccMsg(71, 1), { activeBank: POT_BANKS.ABLETON }); // delta +1 → value 1
    handleMoveKnobs(ccMsg(71, 1), { activeBank: POT_BANKS.ABLETON }); // delta +1 → value 2
    assertEqual(mock.sentExternal[1][3], 2, 'ABLETON absolute: accumulated value sent');

    // Relative mode: raw value sent outgoing, state still accumulates
    const { handleMoveKnobs: hk2, configurePotBank: cfg2, POT_BANKS: PB2 } = await freshKnobs();
    cfg2(PB2.ABLETON, { encoderMode: 'relative' });
    mock.clearAll();
    hk2(ccMsg(71, 1), { activeBank: PB2.ABLETON }); // delta +1, raw=1
    hk2(ccMsg(71, 1), { activeBank: PB2.ABLETON }); // delta +1, raw=1
    // In relative mode, sentValue = raw input (1), not accumulated
    assertEqual(mock.sentExternal[0][3], 1, 'ABLETON relative: raw value 1 sent first turn');
    assertEqual(mock.sentExternal[1][3], 1, 'ABLETON relative: raw value 1 sent second turn');
}

async function testTrackScopedPots() {
    console.log('\n--- Track-scoped pots ---');
    const mock = createHostMock();
    const { handleMoveKnobs, setPotTrack, setTrackScopedPots, POT_BANKS } = await freshKnobs();

    // Track 0, potIndex 0, trackScoped on: CC out = 71 + 0 + 10*0 = 71
    setTrackScopedPots(true);
    setPotTrack(0);
    mock.clearAll();
    const r0 = handleMoveKnobs(ccMsg(71, 5));
    assertEqual(r0.cc, 71, 'track-scoped: track 0 potIndex 0 → CC 71');

    // Track 2, potIndex 0: CC out = 71 + 0 + 10*2 = 91
    setPotTrack(2);
    mock.clearAll();
    const r2 = handleMoveKnobs(ccMsg(71, 5));
    assertEqual(r2.cc, 91, 'track-scoped: track 2 potIndex 0 → CC 91');

    // Different tracks should not share accumulated values
    setPotTrack(0);
    const { handleMoveKnobs: hk2, setPotTrack: spt2, setTrackScopedPots: sts2 } = await freshKnobs();
    sts2(true);
    spt2(0);
    mock.clearAll();
    hk2(ccMsg(71, 5)); // track 0 → value 5
    spt2(1);
    mock.clearAll();
    hk2(ccMsg(71, 5)); // track 1 → value 5 (starts at 0, not 5 from track 0)
    assertEqual(mock.sentExternal[0][3], 5, 'track-scoped: tracks have independent state');

    // Non-scoped: all tracks use same CC
    setTrackScopedPots(false);
    mock.clearAll();
    const rNS = handleMoveKnobs(ccMsg(71, 5));
    assertEqual(rNS.cc, 71, 'non-track-scoped: always CC 71 regardless of track');
}

// ── Run all suites ────────────────────────────────────────────────────────────

const mock = createHostMock();

await testBankManagement();
await testConfigurePotBank();
await testM8TrackBank();
await testM8MasterBank();
await testAbletonBankRouting();
await testAbletonEncoderModes();
await testTrackScopedPots();

mock.enableLog(); // restore console before printing results
summarize('move_virtual_knobs');
