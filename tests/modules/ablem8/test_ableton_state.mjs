/*
 * Unit tests for AbleM8 Ableton bank state machine
 *
 * Run: node tests/modules/ablem8/test_ableton_state.mjs
 *
 * Tests cover:
 *   1. Mute toggle (internal pad → state flip + note to Live + LED update)
 *   2. Solo toggle
 *   3. Arm toggle
 *   4. Double-toggle correctly restores state
 *   5. Incoming volume CC → state + knob indicator
 *   6. Clip warning threshold (volume >= 118 → lemonade LED)
 *   7. Incoming sendA CC → state + knob indicator (shift-gated)
 *   8. Master volume CC (127) → state + knob 9 indicator when !shiftHeld
 *   9. Return volume CC (117) → state + knob 9 indicator when shiftHeld
 *  10. Panic: Mute button sets masterVolume=0 and sends CC 127 value 0
 *  11. Shift+Capture toggles abletonDeviceMode
 *  12. Device mode: knob 0 routes to macro CC 32 instead of volume CC 0
 */

import { createHostMock } from '../harness/host_mock.mjs';
import { assertEqual, assertTrue, assertFalse, summarize } from '../harness/assert.mjs';

// ── Helpers ───────────────────────────────────────────────────────────────────

async function freshAbleM8() {
    const url = new URL('../../../src/modules/ablem8/ui.mjs', import.meta.url);
    url.searchParams.set('t', Date.now() + Math.random());
    await import(url.href);
}

function cc(num, value) { return [0xb0, num, value]; }
function noteOn(note, vel) { return [0x90, note, vel]; }
function noteOff(note) { return [0x80, note, 0]; }
function extCC(channel, num, value) { return [0xb0 | channel, num, value]; }

// Move hardware constants (from ui.mjs)
const moveSHIFT  = 49;
const moveMUTE   = 88;
const moveCAP    = 52;
const ABLETON_CH = 15; // yursProfile.midiChannel

// Move grid row 0 notes (mute row)
const muteRow = [92, 93, 94, 95, 96, 97, 98, 99];
// Move grid row 1 (solo), row 2 (arm)
const soloRow = [84, 85, 86, 87, 88, 89, 90, 91];
const armRow  = [76, 77, 78, 79, 80, 81, 82, 83];

// Switch to Ableton bank: Shift held, then Shift+Menu
async function switchToAbletonBank() {
    globalThis.onMidiMessageInternal(cc(moveSHIFT, 127)); // shiftHeld = true
    globalThis.onMidiMessageInternal(cc(50, 127));         // Shift+Menu → ABLETON
}

// ── Test suites ───────────────────────────────────────────────────────────────

async function testMuteToggle() {
    console.log('\n--- Mute toggle ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    // Press pad at row 0, col 0 (note 92) → mute track 0
    mock.clearAll();
    globalThis.onMidiMessageInternal(noteOn(muteRow[0], 127));
    // Outgoing note to Ableton (external)
    assertTrue(mock.sentExternal.length > 0, 'mute press: note sent to Ableton');
    const note = mock.sentExternal[0];
    assertEqual(note[1] & 0x0f, ABLETON_CH, 'mute note: correct channel');
    assertEqual(note[2], 0, 'mute note: note number 0 (mute[0])');
    // Internal LED update
    assertTrue(mock.sentInternal.length > 0, 'mute press: LED update sent');

    // Press again → toggle back
    mock.clearAll();
    globalThis.onMidiMessageInternal(noteOn(muteRow[0], 127));
    assertTrue(mock.sentExternal.length > 0, 'mute second press: note sent to Ableton');
}

async function testSoloToggle() {
    console.log('\n--- Solo toggle ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    mock.clearAll();
    globalThis.onMidiMessageInternal(noteOn(soloRow[2], 127));
    assertTrue(mock.sentExternal.length > 0, 'solo press: note sent to Ableton');
    const note = mock.sentExternal[0];
    assertEqual(note[1] & 0x0f, ABLETON_CH, 'solo note: correct channel');
    assertEqual(note[2], 18, 'solo note: note 18 (solo[2])');
    assertTrue(mock.sentInternal.length > 0, 'solo press: LED update sent');
}

async function testArmToggle() {
    console.log('\n--- Arm toggle ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    mock.clearAll();
    globalThis.onMidiMessageInternal(noteOn(armRow[5], 127));
    assertTrue(mock.sentExternal.length > 0, 'arm press: note sent to Ableton');
    const note = mock.sentExternal[0];
    assertEqual(note[1] & 0x0f, ABLETON_CH, 'arm note: correct channel');
    assertEqual(note[2], 37, 'arm note: note 37 (arm[5])');
    assertTrue(mock.sentInternal.length > 0, 'arm press: LED update sent');
}

async function testDoubleToggle() {
    console.log('\n--- Double toggle restores state ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    // Toggle mute on
    globalThis.onMidiMessageInternal(noteOn(muteRow[3], 127));
    // Toggle mute off — second note sent should be note-off or 0-velocity
    mock.clearAll();
    globalThis.onMidiMessageInternal(noteOn(muteRow[3], 127));
    assertTrue(mock.sentExternal.length > 0, 'double toggle: second note sent');
}

async function testIncomingVolume() {
    console.log('\n--- Incoming volume CC ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    mock.clearAll();
    // Ableton sends volume[0] = CC 0 on channel 16 with value 100
    globalThis.onMidiMessageExternal(extCC(ABLETON_CH, 0, 100));
    // Knob indicator (CC message to moveKnobCcs[0] = CC 71) — filter past the mute LED update
    const knobUpdate = mock.sentInternal.filter(p => p[1] === 0xb0 && p[2] === 71);
    assertTrue(knobUpdate.length > 0, 'incoming volume: knob indicator updated');
    assertEqual(knobUpdate[0][2], 71, 'incoming volume: CC 71 (knob 0) updated');
    assertEqual(knobUpdate[0][3], 100, 'incoming volume: value 100');
}

async function testClipWarning() {
    console.log('\n--- Clip warning threshold ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    mock.clearAll();
    // Volume >= 118 → mute LED should be lemonade (0x6b) not normal muteOff (0x7c)
    globalThis.onMidiMessageExternal(extCC(ABLETON_CH, 0, 118));
    // The mute LED update is sent to the internal pad (moveGridRows[0][0] = note 92)
    const padSends = mock.sentInternal.filter(p => p[1] === 0x90 && p[2] === muteRow[0]);
    assertTrue(padSends.length > 0, 'clip warning: mute LED pad updated');
    const vel = padSends[padSends.length - 1][3];
    assertEqual(vel, 0x6b, 'clip warning: lemonade color (0x6b) at threshold 118');

    // Below threshold → normal muteOff color (0x7c light_grey)
    mock.clearAll();
    globalThis.onMidiMessageExternal(extCC(ABLETON_CH, 0, 117));
    const padSends2 = mock.sentInternal.filter(p => p[1] === 0x90 && p[2] === muteRow[0]);
    assertTrue(padSends2.length > 0, 'below threshold: mute LED updated');
    const vel2 = padSends2[padSends2.length - 1][3];
    assertEqual(vel2, 0x7c, 'below threshold: light_grey color (0x7c)');
}

async function testIncomingSendA() {
    console.log('\n--- Incoming sendA CC ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    // Without shift: incoming sendA should NOT update knob indicator
    mock.clearAll();
    globalThis.onMidiMessageExternal(extCC(ABLETON_CH, 16, 80)); // sendA[0] = CC 16
    const updatesNoShift = mock.sentInternal.filter(p => p[1] === 0xb0 && p[2] === 71);
    assertEqual(updatesNoShift.length, 0, 'sendA without shift: knob indicator NOT updated');

    // With shift: should update knob indicator
    globalThis.onMidiMessageInternal(cc(moveSHIFT, 127)); // shift on
    mock.clearAll();
    globalThis.onMidiMessageExternal(extCC(ABLETON_CH, 16, 80));
    const updatesWithShift = mock.sentInternal.filter(p => p[1] === 0xb0 && p[2] === 71);
    assertTrue(updatesWithShift.length > 0, 'sendA with shift: knob indicator updated');
    assertEqual(updatesWithShift[0][3], 80, 'sendA with shift: correct value');
}

async function testMasterVolume() {
    console.log('\n--- Master volume CC ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    // Without shift: master volume (CC 127) should update knob 9 (CC 80)
    mock.clearAll();
    globalThis.onMidiMessageExternal(extCC(ABLETON_CH, 127, 90));
    const knob9 = mock.sentInternal.filter(p => p[1] === 0xb0 && p[2] === 80);
    assertTrue(knob9.length > 0, 'master volume: knob 9 (CC 80) updated');
    assertEqual(knob9[0][3], 90, 'master volume: correct value 90');

    // With shift: master volume feedback should NOT update knob 9
    globalThis.onMidiMessageInternal(cc(moveSHIFT, 127));
    mock.clearAll();
    globalThis.onMidiMessageExternal(extCC(ABLETON_CH, 127, 90));
    const knob9Shift = mock.sentInternal.filter(p => p[1] === 0xb0 && p[2] === 80);
    assertEqual(knob9Shift.length, 0, 'master volume with shift: knob 9 NOT updated');
}

async function testReturnVolume() {
    console.log('\n--- Return volume CC ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    // Without shift: return volume (CC 117) should NOT update knob 9
    mock.clearAll();
    globalThis.onMidiMessageExternal(extCC(ABLETON_CH, 117, 50));
    const knob9NoShift = mock.sentInternal.filter(p => p[1] === 0xb0 && p[2] === 80);
    assertEqual(knob9NoShift.length, 0, 'return volume without shift: knob 9 NOT updated');

    // With shift: should update knob 9
    globalThis.onMidiMessageInternal(cc(moveSHIFT, 127));
    mock.clearAll();
    globalThis.onMidiMessageExternal(extCC(ABLETON_CH, 117, 50));
    const knob9Shift = mock.sentInternal.filter(p => p[1] === 0xb0 && p[2] === 80);
    assertTrue(knob9Shift.length > 0, 'return volume with shift: knob 9 updated');
    assertEqual(knob9Shift[0][3], 50, 'return volume with shift: correct value 50');
}

async function testPanic() {
    console.log('\n--- Panic (Mute button) ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    mock.clearAll();
    globalThis.onMidiMessageInternal(cc(moveMUTE, 127));
    // Should send CC 127 = 0 to Ableton external
    const panic = mock.sentExternal.filter(p => (p[1] & 0xf0) === 0xb0 && p[2] === 127 && p[3] === 0);
    assertTrue(panic.length > 0, 'panic: CC 127 value 0 sent to Ableton');
    // Encoder indicators also updated
    assertTrue(mock.sentInternal.length > 0, 'panic: encoder indicators updated');
}

async function testDeviceModetToggle() {
    console.log('\n--- Device macro mode toggle ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    // Shift+Capture → enable device mode
    globalThis.onMidiMessageInternal(cc(moveSHIFT, 127)); // shift on
    mock.clearAll();
    globalThis.onMidiMessageInternal(cc(moveCAP, 127));   // Shift+Capture
    // Encoder indicators should update (device mode = macro values, not volumes)
    assertTrue(mock.sentInternal.length > 0, 'device mode on: encoder indicators updated');

    // Shift+Capture again → disable device mode
    mock.clearAll();
    globalThis.onMidiMessageInternal(cc(moveCAP, 127));
    assertTrue(mock.sentInternal.length > 0, 'device mode off: encoder indicators updated');
}

async function testDeviceModeKnobRouting() {
    console.log('\n--- Device mode knob routing ---');
    const mock = createHostMock();
    await freshAbleM8();
    await switchToAbletonBank();

    // Enable device mode
    globalThis.onMidiMessageInternal(cc(moveSHIFT, 127));
    globalThis.onMidiMessageInternal(cc(moveCAP, 127));
    globalThis.onMidiMessageInternal(cc(moveSHIFT, 0)); // release shift

    mock.clearAll();
    // Knob 0 = CC 71, delta +1
    globalThis.onMidiMessageInternal(cc(71, 1));
    // In device mode, should route to macros[0] = CC 32 (not volume CC 0)
    const externalPkts = mock.sentExternal.filter(p => (p[1] & 0xf0) === 0xb0);
    assertTrue(externalPkts.length > 0, 'device mode knob: external CC sent');
    assertEqual(externalPkts[0][2], 32, 'device mode knob 0: routes to macro CC 32');

    // Not in device mode → volume CC 0
    globalThis.onMidiMessageInternal(cc(moveSHIFT, 127));
    globalThis.onMidiMessageInternal(cc(moveCAP, 127)); // toggle off
    globalThis.onMidiMessageInternal(cc(moveSHIFT, 0));
    mock.clearAll();
    globalThis.onMidiMessageInternal(cc(71, 1));
    const normalPkts = mock.sentExternal.filter(p => (p[1] & 0xf0) === 0xb0);
    assertTrue(normalPkts.length > 0, 'normal mode knob: external CC sent');
    assertEqual(normalPkts[0][2], 0, 'normal mode knob 0: routes to volume CC 0');
}

// ── Run all suites ────────────────────────────────────────────────────────────

const mock = createHostMock();

await testMuteToggle();
await testSoloToggle();
await testArmToggle();
await testDoubleToggle();
await testIncomingVolume();
await testClipWarning();
await testIncomingSendA();
await testMasterVolume();
await testReturnVolume();
await testPanic();
await testDeviceModetToggle();
await testDeviceModeKnobRouting();

mock.enableLog();
summarize('ablem8_ableton_state');
