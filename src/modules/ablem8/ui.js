/*
 * AbleM8 — M8 tracker + Ableton Live dual-mode control surface
 *
 * Banks:
 *   M8 Track, M8 Master, M8 FX  — control the Dirtywave M8 via LPP protocol
 *   Ableton (YURS)               — mixer / device control via YURS remote script
 *
 * Bank switching: Shift+Menu toggles between M8 Track and Ableton banks.
 * Exit overtake:  Shift+Vol+Jog (host-level shortcut, always works).
 *
 * YURS reference: https://forum.yaeltex.com/t/yurs-yaeltex-universal-remote-script-for-ableton-live/161
 * LPP3 reference: https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/downloads/LPP3_prog_ref_guide_200415.pdf
 */

import {
    handleMoveKnobs,
    setPotBank,
    setPotTrack,
    setTrackScopedPots,
    setPotShiftHeld,
    configurePotBank,
    POT_BANKS,
} from '/data/UserData/move-anything/shared/move_virtual_knobs.mjs';

import { ablem8Config } from './config/ablem8_config.mjs';

/* ── Inline helpers ──────────────────────────────────────────────────────── */

function clamp(value, min, max) {
    return Math.min(Math.max(value, min), max);
}

function clampMidi(value) {
    return clamp(value, 0, 127);
}

// Poly aftertouch → modwheel on external channel 3
function aftertouchToModwheel(data, channel = 3) {
    if (data[0] !== 0xa0) return false;
    move_midi_external_send([(2 << 4) | 0xb, 0xb0 | channel, 1, data[2]]);
    return true;
}

/* ── LPP note grid ───────────────────────────────────────────────────────── */

const lppNotes = [
    90, 91, 92, 93, 94, 95, 96, 97, 98, 99,
    80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
    70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
    60, 61, 62, 63, 64, 65, 66, 67, 68, 69,
    50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
    40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    101, 102, 103, 104, 105, 106, 107, 108,
    1, 2, 3, 4, 5, 6, 7, 8
];

const lppNoteValueMap = new Map([...lppNotes.map((a) => [a, [0, 0, 0]])]);

/* ── Move ↔ LPP control mappings ────────────────────────────────────────── */

const moveControlToLppNoteMapTop = new Map([
    [55, 80], [54, 70], [62, 91], [63, 92], [85, 20],
    [43, 89], [42, 79], [41, 69], [40, 59],
    [50, 94], [49, 90], [119, 60], [51, 93], [52, 97],
    [88, 2], [56, 1], [86, 10], [60, 50], [58, 3],
    [118, 98], [99, 99]
]);

const lppNoteToMoveControlMapTop = new Map(
    [...moveControlToLppNoteMapTop.entries()].map((a) => [a[1], a[0]])
);

const moveControlToLppNoteMapBottom = new Map([
    [55, 80], [54, 70], [62, 91], [63, 92], [85, 20],
    [43, 49], [42, 39], [41, 29], [40, 19],
    [50, 94], [49, 90], [119, 60], [51, 93], [52, 97],
    [88, 2], [56, 1], [86, 10], [60, 50], [58, 3],
    [118, 98], [99, 99]
]);

const lppNoteToMoveControlMapBottom = new Map(
    [...moveControlToLppNoteMapBottom.entries()].map((a) => [a[1], a[0]])
);

/* ── Move ↔ LPP pad mappings ─────────────────────────────────────────────── */

const lppPadToMovePadMapTop = new Map([
    [81, 92], [82, 93], [83, 94], [84, 95], [85, 96], [86, 97], [87, 98], [88, 99],
    [71, 84], [72, 85], [73, 86], [74, 87], [75, 88], [76, 89], [77, 90], [78, 91],
    [61, 76], [62, 77], [63, 78], [64, 79], [65, 80], [66, 81], [67, 82], [68, 83],
    [51, 68], [52, 69], [53, 70], [54, 71], [55, 72], [56, 73], [57, 74], [58, 75],
    [101, 16], [102, 18], [103, 20], [104, 22], [105, 24], [106, 26], [107, 28], [108, 30]
]);

const moveToLppPadMapTop = new Map(
    [...lppPadToMovePadMapTop.entries()].map((a) => [a[1], a[0]])
);

const lppPadToMovePadMapBottom = new Map([
    [41, 92], [42, 93], [43, 94], [44, 95], [45, 96], [46, 97], [47, 98], [48, 99],
    [31, 84], [32, 85], [33, 86], [34, 87], [35, 88], [36, 89], [37, 90], [38, 91],
    [21, 76], [22, 77], [23, 78], [24, 79], [25, 80], [26, 81], [27, 82], [28, 83],
    [11, 68], [12, 69], [13, 70], [14, 71], [15, 72], [16, 73], [17, 74], [18, 75],
    [101, 16], [102, 18], [103, 20], [104, 22], [105, 24], [106, 26], [107, 28], [108, 30]
]);

const moveToLppPadMapBottom = new Map(
    [...lppPadToMovePadMapBottom.entries()].map((a) => [a[1], a[0]])
);

/* ── LED color palette ───────────────────────────────────────────────────── */

const light_grey = 0x7c;
const dim_grey   = 0x10;
const green      = 0x7e;
const navy       = 0x7d;
const sky        = 0x5f;
const red        = 0x7f;
const azure      = 0x63;
const white      = 0x7a;
const pink       = 0x6d;
const aqua       = 0x5a;
const black      = 0x00;
const lemonade   = 0x6b;
const lime       = 0x20;
const fern       = 0x55;

const lppColorToMoveColorMap = new Map([
    [0x15, green], [0x17, lime], [0x1, light_grey], [0x05, red], [0x03, white],
    [0x4e, sky], [0x47, pink], [0x13, aqua], [0x27, navy], [0x2b, azure], [0x16, fern]
]);

const lppColorToMoveMonoMap = new Map([
    [0x05, 0x7f], [0x78, 0x7f], [0x01, 0x10], [0x07, 0x0f]
]);

/* ── Move hardware CC/note constants ─────────────────────────────────────── */

const moveLOGO    = 99;
const moveMENU    = 50;
const moveBACK    = 51;
const moveCAP     = 52;
const moveSHIFT   = 49;
const moveWHEEL   = 3;
const movePLAY    = 85;
const moveREC     = 86;
const moveLOOP    = 58;
const moveMUTE    = 88;
const moveUNDO    = 56;
const moveTRACK1  = 16;
const moveSAMPLE  = 118;
const moveWHEELTouch = 9;

/* ── Bank aliases ────────────────────────────────────────────────────────── */

const BANK_M8_TRACK  = POT_BANKS.M8_TRACK;
const BANK_M8_MASTER = POT_BANKS.M8_MASTER;
const BANK_M8_FX     = POT_BANKS.M8_FX;
const BANK_ABLETON   = POT_BANKS.ABLETON;

function isM8Bank(bank) {
    return bank === BANK_M8_TRACK || bank === BANK_M8_MASTER || bank === BANK_M8_FX;
}

/* ── Move grid layout ────────────────────────────────────────────────────── */

const moveGridRows = [
    [92, 93, 94, 95, 96, 97, 98, 99],
    [84, 85, 86, 87, 88, 89, 90, 91],
    [76, 77, 78, 79, 80, 81, 82, 83],
    [68, 69, 70, 71, 72, 73, 74, 75]
];

const moveKnobCcs = [71, 72, 73, 74, 75, 76, 77, 78, 79, 80];

/* ── YURS / Ableton config ───────────────────────────────────────────────── */

const { yursProfile, clipWarningThreshold } = ablem8Config;

configurePotBank(POT_BANKS.ABLETON, {
    channel: yursProfile.midiChannel,
    volumeCcs: yursProfile.ccs.volume,
    sendACcs: yursProfile.ccs.sendA,
    masterCc: yursProfile.ccs.masterVolume,
    returnCc: yursProfile.ccs.returnVolume,
    encoderMode: yursProfile.encoderMode,
});

const abletonState = {
    volumes:      new Array(8).fill(64),
    sendsA:       new Array(8).fill(0),
    mutes:        new Array(8).fill(false),
    solos:        new Array(8).fill(false),
    arms:         new Array(8).fill(false),
    masterVolume: 64,
    returnVolume: 0,
    macros:       new Array(8).fill(0)
};

const abletonColors = {
    muteOn:      red,
    muteOff:     light_grey,
    muteClip:    lemonade,
    soloOn:      lime,
    soloOff:     dim_grey,
    armOn:       aqua,
    armOff:      dim_grey,
    bankM8:      green,
    bankAbleton: azure,
    panic:       red
};

const bankIndicatorNote = 31; // rightmost step LED

/* ── Module state ────────────────────────────────────────────────────────── */

let showingTop        = true;
let activeBank        = BANK_M8_TRACK;
let abletonDeviceMode = false;
let selectedTrackIndex = 0;
let shiftHeld         = false;
let liveMode          = false;
let isPlaying         = false;
let currentView       = moveBACK;
let wheelClicked      = false;
let abletonMacroValues = new Array(8).fill(0);
let sysexBuffer       = [];

setPotBank(activeBank);

/* ── Progressive LED queue ───────────────────────────────────────────────── */

const LEDS_PER_FRAME = 8;
let ledQueue = [];

function drainLedQueue() {
    const count = Math.min(LEDS_PER_FRAME, ledQueue.length);
    for (let i = 0; i < count; i++) {
        ledQueue.shift()();
    }
}

function buildClearQueue() {
    ledQueue = [];
    for (let note = 16; note <= 31; note++) {
        ledQueue.push(() => sendMovePad(note, 0));
    }
    for (const row of moveGridRows) {
        for (const note of row) {
            ledQueue.push(() => sendMovePad(note, 0));
        }
    }
    const controls = [moveSHIFT, moveMENU, moveBACK, moveCAP, movePLAY, moveREC,
                      moveLOOP, moveMUTE, moveUNDO, moveSAMPLE, moveLOGO];
    for (const cc of controls) {
        ledQueue.push(() => sendMoveControl(cc, 0));
    }
    for (const cc of moveKnobCcs) {
        ledQueue.push(() => sendMoveControl(cc, 0));
    }
}

function buildM8BankQueue() {
    for (const data of lppNoteValueMap.values()) {
        if (data[0] === 0 && data[1] === 0 && data[2] === 0) continue;
        const d = data;
        ledQueue.push(() => handleM8ExternalMessage(d));
    }
    ledQueue.push(() => updateMoveViewPulse());
    ledQueue.push(() => updatePLAYLed());
    ledQueue.push(() => drawBankIndicator());
}

function buildAbletonBankQueue() {
    for (let i = 0; i < 8; i++) {
        const idx = i;
        ledQueue.push(() => updateAbletonMuteLed(idx));
        ledQueue.push(() => updateAbletonSoloLed(idx));
        ledQueue.push(() => updateAbletonArmLed(idx));
    }
    for (let i = 0; i < moveKnobCcs.length; i++) {
        const idx = i;
        ledQueue.push(() => {
            const vals = shiftHeld ? abletonState.sendsA : abletonState.volumes;
            if (idx < 8) sendMoveControl(moveKnobCcs[idx], vals[idx]);
            else if (moveKnobCcs[idx] !== undefined) {
                sendMoveControl(moveKnobCcs[idx], shiftHeld ? abletonState.returnVolume : abletonState.masterVolume);
            }
        });
    }
    ledQueue.push(() => sendMoveControl(moveMUTE, abletonColors.panic));
    ledQueue.push(() => drawBankIndicator());
}

/* ── MIDI send helpers ───────────────────────────────────────────────────── */

function sendMovePad(note, color) {
    move_midi_internal_send([0 << 4 | 0x9, 0x90, note, clampMidi(color)]);
}

function sendMoveControl(controlNumber, value) {
    move_midi_internal_send([0 << 4 | 0xb, 0xB0, controlNumber, clampMidi(value)]);
}

function sendExternalMidi(status, data1, data2) {
    const cin = (status & 0xf0) >> 4;
    move_midi_external_send([2 << 4 | cin, status, data1, data2]);
}

function sendAbletonCC(cc, value) {
    sendExternalMidi(0xb0 | yursProfile.midiChannel, cc, clampMidi(value));
}

function sendAbletonMacroCC(index, value) {
    const macroCcs = yursProfile.ccs.macros ?? [];
    if (index < 0 || index >= macroCcs.length) return;
    sendAbletonCC(macroCcs[index], value);
}

function sendAbletonNote(note, pressed) {
    const status = (pressed ? 0x90 : 0x80) | yursProfile.midiChannel;
    const velocity = pressed ? yursProfile.feedback.noteOn : yursProfile.feedback.noteOff;
    sendExternalMidi(status, note, clampMidi(velocity));
}

/* ── Display helpers ─────────────────────────────────────────────────────── */

function drawAbletonMacroDisplay() {
    if (!abletonDeviceMode) return;
    if (typeof clear_screen !== "function" || typeof print !== "function") return;
    clear_screen();
    print(0, 0, `Ableton Dev T${selectedTrackIndex + 1}`, 1);
    for (let i = 0; i < 8; i++) {
        const line = Math.floor(i / 4);
        const col  = i % 4;
        print(col * 30, 16 + line * 16, `M${i + 1}:${abletonMacroValues[i].toString().padStart(3, " ")}`, 1);
    }
}

/* ── LED draw functions ──────────────────────────────────────────────────── */

function drawBankIndicator() {
    const color = isM8Bank(activeBank) ? abletonColors.bankM8 : abletonColors.bankAbleton;
    sendMovePad(bankIndicatorNote, color);
}

function updateMoveViewPulse() {
    sendMoveControl(moveBACK,  dim_grey);
    sendMoveControl(moveMENU,  dim_grey);
    sendMoveControl(moveCAP,   dim_grey);
    sendMoveControl(currentView, light_grey);
    if (!showingTop) {
        move_midi_internal_send([0 << 4 | 0xb, 0xBA, currentView, black]);
    }
}

function updatePLAYLed() {
    if (!liveMode && !isPlaying) { sendMoveControl(movePLAY, light_grey); return; }
    if (!liveMode &&  isPlaying) { sendMoveControl(movePLAY, green);      return; }
    if ( liveMode && !isPlaying) { sendMoveControl(movePLAY, sky);        return; }
    if ( liveMode &&  isPlaying) { sendMoveControl(movePLAY, navy);       return; }
}

function updateAbletonMuteLed(index) {
    let color = abletonState.mutes[index] ? abletonColors.muteOn : abletonColors.muteOff;
    if (!abletonState.mutes[index] && abletonState.volumes[index] >= clipWarningThreshold) {
        color = abletonColors.muteClip;
    }
    sendMovePad(moveGridRows[0][index], color);
}

function updateAbletonSoloLed(index) {
    const color = abletonState.solos[index] ? abletonColors.soloOn : abletonColors.soloOff;
    sendMovePad(moveGridRows[1][index], color);
}

function updateAbletonArmLed(index) {
    const color = abletonState.arms[index] ? abletonColors.armOn : abletonColors.armOff;
    sendMovePad(moveGridRows[2][index], color);
}

function updateAbletonEncoderIndicators() {
    if (abletonDeviceMode) {
        for (let i = 0; i < 8; i++) sendMoveControl(moveKnobCcs[i], abletonMacroValues[i]);
        return;
    }
    const values = shiftHeld ? abletonState.sendsA : abletonState.volumes;
    for (let i = 0; i < 8; i++) sendMoveControl(moveKnobCcs[i], values[i]);
    const masterValue = shiftHeld ? abletonState.returnVolume : abletonState.masterVolume;
    if (moveKnobCcs[9] !== undefined) sendMoveControl(moveKnobCcs[9], masterValue);
}

/* ── LPP init ────────────────────────────────────────────────────────────── */

function initLPP() {
    const out_cable = 2;
    const LPPInitSysex = [
        out_cable << 4 | 0x4, 0xF0, 126, 0,
        out_cable << 4 | 0x4, 6, 2, 0,
        out_cable << 4 | 0x4, 32, 41, 0x00,
        out_cable << 4 | 0x4, 0x00, 0x00, 0x00,
        out_cable << 4 | 0x4, 0x00, 0x00, 0x00,
        out_cable << 4 | 0x6, 0x00, 0xF7, 0x0
    ];
    console.log("AbleM8: sending LPP init");
    move_midi_external_send(LPPInitSysex);
    showingTop = true;
}

/* ── Bank switching ──────────────────────────────────────────────────────── */

function releaseM8Shift() {
    const lppShiftNote = moveControlToLppNoteMapTop.get(moveSHIFT);
    if (lppShiftNote !== undefined) sendExternalMidi(0x80, lppShiftNote, 0);
}

function resetModifiersForBankSwitch() {
    shiftHeld    = false;
    wheelClicked = false;
    showingTop   = true;
    setPotShiftHeld(false);
}

function setActiveBank(nextBank) {
    if (activeBank === nextBank) return;

    if (isM8Bank(activeBank) && shiftHeld) releaseM8Shift();

    activeBank = nextBank;
    setPotBank(nextBank);
    resetModifiersForBankSwitch();

    // Build a queued transition: clear all LEDs, then redraw new bank
    buildClearQueue();

    if (isM8Bank(activeBank)) {
        ledQueue.push(() => initLPP());
        buildM8BankQueue();
    } else {
        ledQueue.push(() => {
            abletonDeviceMode = false;
            if (typeof clear_screen === "function") clear_screen();
        });
        buildAbletonBankQueue();
    }

    console.log(`AbleM8: active bank → ${activeBank}`);
}

/* ── Ableton device mode ─────────────────────────────────────────────────── */

function setAbletonDeviceMode(enabled) {
    abletonDeviceMode = enabled;
    updateAbletonEncoderIndicators();
    if (abletonDeviceMode) {
        drawAbletonMacroDisplay();
    } else if (typeof clear_screen === "function") {
        clear_screen();
    }
}

/* ── Utility ─────────────────────────────────────────────────────────────── */

function findIndex(list, value) {
    for (let i = 0; i < list.length; i++) {
        if (list[i] === value) return i;
    }
    return -1;
}

function arraysAreEqual(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
        if (a[i] !== b[i]) return false;
    }
    return true;
}

function mapYursColor(velocity) {
    return lppColorToMoveColorMap.get(velocity) ?? velocity;
}

function decodeMoveKnobDelta(value) {
    if (value === 1)   return 1;
    if (value === 127) return -1;
    if (value > 1 && value < 64)  return value;
    if (value > 64) return value - 128;
    return 0;
}

function updateEncoderValue(currentValue, rawValue) {
    const delta = decodeMoveKnobDelta(rawValue);
    if (delta !== 0) return clampMidi(currentValue + delta);
    return clampMidi(rawValue);
}

function updateSelectedTrack(index) {
    selectedTrackIndex = clamp(index, 0, 7);
    setPotTrack(selectedTrackIndex);
}

function updateMovePadsToMatchLpp() {
    const activeMoveToLppPadMap = showingTop ? moveToLppPadMapTop : moveToLppPadMapBottom;
    for (const [, lppPad] of activeMoveToLppPadMap.entries()) {
        const data = lppNoteValueMap.get(lppPad);
        handleM8ExternalMessage(data);
    }
}

/* ── External message handlers ───────────────────────────────────────────── */

const m8InitSysex = [0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7];

function handleM8ExternalMessage(data) {
    if (!data || data[0] === 0xf8) return; // ignore MIDI clock

    const value      = data[0];
    const maskedValue = value & 0xf0;
    const noteOn     = maskedValue === 0x90;
    const noteOff    = maskedValue === 0x80;

    // Sysex accumulator (QuickJS delivers sysex as 3-byte slices)
    const sysexStart = value === 0xF0;
    const sysexEnd   = data[2] === 0xF7;

    if (sysexStart) {
        sysexBuffer = [...data];
        return;
    }
    if (sysexBuffer.length && !sysexEnd) {
        sysexBuffer.push(...data);
        return;
    }
    if (sysexEnd) {
        sysexBuffer.push(...data);
        if (arraysAreEqual(sysexBuffer, m8InitSysex)) initLPP();
        sysexBuffer = [];
        return;
    }

    if (!(noteOn || noteOff)) return;

    const lppNoteNumber = data[1];
    const lppVelocity   = data[2];

    lppNoteValueMap.set(lppNoteNumber, [...data]);

    const activeLppToMovePadMap = showingTop ? lppPadToMovePadMapTop : lppPadToMovePadMapBottom;
    const moveNoteNumber = activeLppToMovePadMap.get(lppNoteNumber);
    const moveVelocity   = lppColorToMoveColorMap.get(lppVelocity) ?? lppVelocity;

    if (moveNoteNumber) {
        if (value === 0x91 && moveVelocity !== 0) {
            move_midi_internal_send([0 << 4 | (value / 16), 0x9f, moveNoteNumber, moveVelocity]);
        } else {
            move_midi_internal_send([0 << 4 | (maskedValue / 16), maskedValue, moveNoteNumber, moveVelocity]);
            if (value === 0x92 && moveVelocity !== 0) {
                move_midi_internal_send([0 << 4 | (value / 16), 0x9a, moveNoteNumber, light_grey]);
            }
        }
        return;
    }

    const activeLppToMoveControlMap = showingTop ? lppNoteToMoveControlMapTop : lppNoteToMoveControlMapBottom;
    const moveControlNumber = activeLppToMoveControlMap.get(lppNoteNumber);

    if (moveControlNumber === moveLOGO) {
        liveMode = moveVelocity > 0;
        updatePLAYLed();
        return;
    }
    if (moveControlNumber === movePLAY) {
        isPlaying = moveVelocity === green;
        updatePLAYLed();
        return;
    }

    // Mono-color overrides for certain controls
    let finalVelocity = moveVelocity;
    if (moveControlNumber === moveLOOP || moveControlNumber === moveMUTE || moveControlNumber === moveUNDO) {
        finalVelocity = lppColorToMoveMonoMap.get(lppVelocity) ?? lppVelocity;
    }

    if (moveControlNumber) {
        move_midi_internal_send([0 << 4 | 0xb, 0xB0, moveControlNumber, finalVelocity]);
        if (value === 0x91) {
            move_midi_internal_send([0 << 4 | 0xb, 0xbe, moveControlNumber, black]);
        }
    }
}

function handleAbletonExternalMessage(data) {
    if (!data || data[0] === 0xf8) return;

    const status  = data[0];
    const type    = status & 0xf0;
    const channel = status & 0x0f;

    if (channel !== yursProfile.midiChannel) return;

    if (type === 0xb0) {
        const cc    = data[1];
        const value = clampMidi(data[2]);

        let index = findIndex(yursProfile.ccs.volume, cc);
        if (index !== -1) {
            abletonState.volumes[index] = value;
            updateAbletonMuteLed(index);
            if (!shiftHeld) sendMoveControl(moveKnobCcs[index], value);
            return;
        }

        index = findIndex(yursProfile.ccs.sendA, cc);
        if (index !== -1) {
            abletonState.sendsA[index] = value;
            if (shiftHeld) sendMoveControl(moveKnobCcs[index], value);
            return;
        }

        if (cc === yursProfile.ccs.masterVolume) {
            abletonState.masterVolume = value;
            if (!shiftHeld && moveKnobCcs[9] !== undefined) sendMoveControl(moveKnobCcs[9], value);
            return;
        }

        if (cc === yursProfile.ccs.returnVolume) {
            abletonState.returnVolume = value;
            if (shiftHeld && moveKnobCcs[9] !== undefined) sendMoveControl(moveKnobCcs[9], value);
            return;
        }

        index = findIndex(yursProfile.ccs.macros, cc);
        if (index !== -1) {
            abletonMacroValues[index] = value;
            if (abletonDeviceMode) {
                sendMoveControl(moveKnobCcs[index], value);
                drawAbletonMacroDisplay();
            }
            return;
        }
    }

    if (type === 0x90 || type === 0x80) {
        const note     = data[1];
        const velocity = data[2];
        const pressed  = type === 0x90 && velocity > 0;

        // LED color feedback from Live
        let rowIndex = -1;
        let colIndex = -1;
        for (let i = 0; i < moveGridRows.length; i++) {
            const idx = findIndex(moveGridRows[i], note);
            if (idx !== -1) { rowIndex = i; colIndex = idx; break; }
        }
        if (rowIndex !== -1) sendMovePad(moveGridRows[rowIndex][colIndex], mapYursColor(velocity));

        let index = findIndex(yursProfile.notes.mute, note);
        if (index !== -1) {
            abletonState.mutes[index] = pressed;
            updateAbletonMuteLed(index);
            return;
        }
        index = findIndex(yursProfile.notes.solo, note);
        if (index !== -1) {
            abletonState.solos[index] = pressed;
            updateAbletonSoloLed(index);
            return;
        }
        index = findIndex(yursProfile.notes.arm, note);
        if (index !== -1) {
            abletonState.arms[index] = pressed;
            updateAbletonArmLed(index);
            return;
        }
    }
}

/* ── Internal message handlers ───────────────────────────────────────────── */

function handleM8InternalMessage(data) {
    const isNote = data[0] === 0x80 || data[0] === 0x90;
    const isCC   = data[0] === 0xb0;
    const isAt   = data[0] === 0xa0;

    if (isAt) {
        aftertouchToModwheel(data);
        return;
    }

    if (!(isNote || isCC)) return;

    if (isNote) {
        const activeMoveToLppPadMap = showingTop ? moveToLppPadMapTop : moveToLppPadMapBottom;
        const moveNoteNumber = data[1];

        // Track selection via step buttons (notes 16–31, every other note = track)
        const trackOffset = moveNoteNumber - moveTRACK1;
        if (trackOffset >= 0 && trackOffset <= 20 && data[2] > 0) {
            const trackIndex = Math.floor(trackOffset / 2);
            if (trackIndex >= 0 && trackIndex <= 7) setPotTrack(trackIndex);
        }

        // Wheel touch: toggle top/bottom view
        if (moveNoteNumber === moveWHEELTouch && data[2] === 127) {
            showingTop = !showingTop;
            updateMovePadsToMatchLpp();
            updateMoveViewPulse();
            return;
        }
        if (moveNoteNumber === moveWHEELTouch && data[2] === 0) {
            if (!wheelClicked) {
                showingTop = !showingTop;
                updateMovePadsToMatchLpp();
                updateMoveViewPulse();
            }
            wheelClicked = false;
            return;
        }

        const lppNote = activeMoveToLppPadMap.get(moveNoteNumber);
        if (!lppNote) return;

        let moveVelocity = clampMidi(data[2] * 2);
        sendExternalMidi(data[0], lppNote, moveVelocity);
        return;
    }

    if (isCC) {
        const moveControlNumber = data[1];
        const activeMoveControlToLppNoteMap = showingTop ? moveControlToLppNoteMapTop : moveControlToLppNoteMapBottom;
        const lppNote = activeMoveControlToLppNoteMap.get(moveControlNumber);

        // Track the active view button
        if (moveControlNumber === moveBACK || moveControlNumber === moveMENU || moveControlNumber === moveCAP) {
            currentView = moveControlNumber;
            updateMoveViewPulse();
        }

        // Wheel click: flag for top/bottom toggle suppression
        if (moveControlNumber === moveWHEEL && data[2] === 0x7f) {
            wheelClicked = true;
            return;
        }

        if (!lppNote) {
            handleMoveKnobs(data);
            return;
        }

        const pressed = data[2] === 127;
        sendExternalMidi(pressed ? 0x90 : 0x80, lppNote, pressed ? 100 : 0);
    }
}

function handleAbletonInternalMessage(data) {
    const status = data[0];
    const type   = status & 0xf0;

    if (type === 0x90 || type === 0x80) {
        const note     = data[1];
        const velocity = data[2];
        const pressed  = type === 0x90 && velocity > 0;

        if (note < 10) return;

        let rowIndex = -1;
        let colIndex = -1;
        for (let i = 0; i < moveGridRows.length; i++) {
            const index = findIndex(moveGridRows[i], note);
            if (index !== -1) { rowIndex = i; colIndex = index; break; }
        }

        if (colIndex !== -1) updateSelectedTrack(colIndex);

        if (rowIndex === 0) {
            sendAbletonNote(yursProfile.notes.mute[colIndex], pressed);
            if (pressed) {
                abletonState.mutes[colIndex] = !abletonState.mutes[colIndex];
                updateAbletonMuteLed(colIndex);
            }
            return;
        }
        if (rowIndex === 1) {
            sendAbletonNote(yursProfile.notes.solo[colIndex], pressed);
            if (pressed) {
                abletonState.solos[colIndex] = !abletonState.solos[colIndex];
                updateAbletonSoloLed(colIndex);
            }
            return;
        }
        if (rowIndex === 2) {
            sendAbletonNote(yursProfile.notes.arm[colIndex], pressed);
            if (pressed) {
                abletonState.arms[colIndex] = !abletonState.arms[colIndex];
                updateAbletonArmLed(colIndex);
            }
            return;
        }
        return;
    }

    if (type === 0xb0) {
        const controlNumber = data[1];
        const value         = data[2];

        // Panic: Mute button clears master volume
        if (!abletonDeviceMode && controlNumber === moveMUTE && value === 0x7f) {
            abletonState.masterVolume = 0;
            sendAbletonCC(yursProfile.ccs.masterVolume, 0);
            updateAbletonEncoderIndicators();
            return;
        }

        // Device macro mode: knobs control macros
        const knobIndex = findIndex(moveKnobCcs, controlNumber);
        if (abletonDeviceMode && knobIndex !== -1) {
            if (knobIndex < 8) {
                const nextValue = updateEncoderValue(abletonMacroValues[knobIndex], value);
                abletonMacroValues[knobIndex] = nextValue;
                sendAbletonMacroCC(knobIndex, nextValue);
                sendMoveControl(moveKnobCcs[knobIndex], nextValue);
                drawAbletonMacroDisplay();
            }
            return;
        }

        const knobHandled = handleMoveKnobs(data, { activeBank: BANK_ABLETON, shiftHeld });
        if (knobHandled && knobHandled.handled) {
            const potIndex = knobHandled.potIndex ?? -1;
            if (potIndex >= 0 && potIndex < 8) {
                if (shiftHeld) abletonState.sendsA[potIndex] = knobHandled.value;
                else           abletonState.volumes[potIndex] = knobHandled.value;
                updateAbletonMuteLed(potIndex);
            } else if (potIndex === 8) {
                if (shiftHeld) abletonState.returnVolume = knobHandled.value;
                else           abletonState.masterVolume = knobHandled.value;
            }
            updateAbletonEncoderIndicators();
        }
    }
}

/* ── Exported module lifecycle ───────────────────────────────────────────── */

globalThis.init = function () {
    console.log("AbleM8: init");
    // Host has already cleared all LEDs. Draw bank indicator + send LPP init.
    // Both are single packets — safe to send immediately.
    drawBankIndicator();
    initLPP();
};

globalThis.tick = function () {
    if (ledQueue.length > 0) {
        drainLedQueue();
    }
};

globalThis.onMidiMessageExternal = function (data) {
    if (isM8Bank(activeBank)) {
        handleM8ExternalMessage(data);
    } else {
        handleAbletonExternalMessage(data);
    }
};

globalThis.onMidiMessageInternal = function (data) {
    const isCC   = data[0] === 0xb0;
    const isNote = data[0] === 0x80 || data[0] === 0x90;

    // Shift tracking (always, regardless of bank)
    if (isCC && data[1] === moveSHIFT) {
        const nextShift = data[2] === 127;
        shiftHeld = nextShift;
        setPotShiftHeld(shiftHeld);
        if (activeBank === BANK_ABLETON) updateAbletonEncoderIndicators();
    }

    // Shift+Capture → Ableton device mode toggle (Ableton bank only)
    if (activeBank === BANK_ABLETON && isCC && data[1] === moveCAP && data[2] === 0x7f && shiftHeld) {
        setAbletonDeviceMode(!abletonDeviceMode);
        return;
    }

    // Shift+Menu → toggle M8 ↔ Ableton bank
    if (isCC && data[1] === moveMENU && data[2] === 0x7f && shiftHeld) {
        const nextBank = activeBank === BANK_ABLETON ? BANK_M8_TRACK : BANK_ABLETON;
        setActiveBank(nextBank);
        return;
    }

    if (isM8Bank(activeBank)) {
        handleM8InternalMessage(data);
        return;
    }

    if (!isNote && !isCC) return;
    handleAbletonInternalMessage(data);
};
