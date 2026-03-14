/*
 * Move Virtual Knobs — multi-bank knob manager
 *
 * Manages the 9 Move encoders across four banks:
 *   M8_TRACK   — M8 track parameters, per-track or global
 *   M8_MASTER  — M8 master parameters
 *   M8_FX      — M8 FX parameters
 *   ABLETON    — Ableton/YURS mixer and device control
 *
 * Host globals used (injected by QuickJS runtime):
 *   move_midi_external_send(packet)
 *   move_midi_internal_send(packet)
 */

function clamp(value, min, max) {
    return Math.min(Math.max(value, min), max);
}

export const POT_BANKS = {
    M8_TRACK: "M8_TRACK",
    M8_MASTER: "M8_MASTER",
    M8_FX: "M8_FX",
    ABLETON: "ABLETON",
};

const defaultBankConfig = {
    [POT_BANKS.M8_TRACK]: { ccBase: 71, potCount: 10, potsPerBank: 10, channel: 3, internalEcho: true },
    [POT_BANKS.M8_MASTER]: { ccBase: 71, potCount: 10, channel: 3, internalEcho: true },
    [POT_BANKS.M8_FX]: { ccBase: 90, potCount: 10, channel: 3, internalEcho: true },
    [POT_BANKS.ABLETON]: {
        ccBase: 71,
        volumeCcs: [0, 1, 2, 3, 4, 5, 6, 7],
        sendACcs: [16, 17, 18, 19, 20, 21, 22, 23],
        masterCc: 127,
        returnCc: 117,
        channel: 15,
        encoderMode: "absolute",
        potCount: 10,
        internalEcho: false,
    },
};

let potConfig = JSON.parse(JSON.stringify(defaultBankConfig));

const potState = {
    activeBank: POT_BANKS.M8_TRACK,
    selectedTrackIndex: 0,
    trackScopedPots: false,
    shiftHeld: false,
};

function makePotStateArrays() {
    const trackCount = 8;
    const state = {};
    state[POT_BANKS.M8_TRACK] = Array(trackCount)
        .fill(0)
        .map(() => Array(potConfig[POT_BANKS.M8_TRACK].potCount).fill(0));
    state[POT_BANKS.M8_MASTER] = [Array(potConfig[POT_BANKS.M8_MASTER].potCount).fill(0)];
    state[POT_BANKS.M8_FX] = [Array(potConfig[POT_BANKS.M8_FX].potCount).fill(0)];
    state[POT_BANKS.ABLETON] = [Array(potConfig[POT_BANKS.ABLETON].potCount).fill(0)];
    return state;
}

let potValues = makePotStateArrays();

export function setPotBank(bank) {
    if (!potConfig[bank]) {
        return;
    }
    potState.activeBank = bank;
}

export function setPotTrack(index) {
    potState.selectedTrackIndex = clamp(index, 0, 7);
}

export function setTrackScopedPots(enabled) {
    potState.trackScopedPots = !!enabled;
}

export function setPotShiftHeld(enabled) {
    potState.shiftHeld = !!enabled;
}

export function configurePotBank(bank, overrides = {}) {
    if (!potConfig[bank]) {
        return;
    }
    potConfig[bank] = { ...potConfig[bank], ...overrides };
    // Rebuild only the affected bank's state to avoid clobbering other banks
    const fresh = makePotStateArrays();
    potValues[bank] = fresh[bank];
}

function decodeDelta(value) {
    if (value === 1) return 1;
    if (value === 127) return -1;
    if (value > 1 && value < 64) return value;
    if (value > 64) return value - 128;
    return 0;
}

function nextPotValue(bank, potIndex, rawValue, trackIndex = 0) {
    const store = potValues[bank][trackIndex] ?? [];
    const current = store[potIndex] ?? 0;

    let next = rawValue;
    const delta = decodeDelta(rawValue);
    if (delta !== 0) {
        next = current + delta;
    }

    next = clamp(next, 0, 127);
    store[potIndex] = next;
    return next;
}

function sendCc(channel, cc, value, mirrorInternal) {
    const safeValue = clamp(value, 0, 127);
    move_midi_external_send([2 << 4 | 0xb, 0xb0 | channel, cc, safeValue]);
    if (mirrorInternal) {
        move_midi_internal_send([0 << 4 | 0xb, 0xb0 | 0, cc, safeValue]);
    }
}

function handleTrackBank(potIndex, value, overrides) {
    const config = potConfig[POT_BANKS.M8_TRACK];
    const channel = overrides.channel ?? config.channel;
    const potsPerBank = config.potsPerBank ?? config.potCount;
    const trackScoped = overrides.trackScoped ?? potState.trackScopedPots;
    const trackIndex = clamp(overrides.selectedTrackIndex ?? potState.selectedTrackIndex, 0, 7);

    const ccBase = config.ccBase;
    const ccOut = trackScoped
        ? ccBase + potIndex + potsPerBank * trackIndex
        : ccBase + potIndex;
    const nextValue = nextPotValue(POT_BANKS.M8_TRACK, potIndex, value, trackIndex);

    sendCc(channel, ccOut, nextValue, config.internalEcho);

    return { handled: true, bank: POT_BANKS.M8_TRACK, potIndex, cc: ccOut, value: nextValue, channel };
}

function handleMasterBank(potIndex, value, overrides) {
    const config = potConfig[POT_BANKS.M8_MASTER];
    const channel = overrides.channel ?? config.channel;
    const ccOut = config.ccBase + potIndex;
    const nextValue = nextPotValue(POT_BANKS.M8_MASTER, potIndex, value);
    sendCc(channel, ccOut, nextValue, config.internalEcho);
    return { handled: true, bank: POT_BANKS.M8_MASTER, potIndex, cc: ccOut, value: nextValue, channel };
}

function handleFxBank(potIndex, value, overrides) {
    const config = potConfig[POT_BANKS.M8_FX];
    const channel = overrides.channel ?? config.channel;
    const ccOut = config.ccBase + potIndex;
    const nextValue = nextPotValue(POT_BANKS.M8_FX, potIndex, value);
    sendCc(channel, ccOut, nextValue, config.internalEcho);
    return { handled: true, bank: POT_BANKS.M8_FX, potIndex, cc: ccOut, value: nextValue, channel };
}

function handleAbletonBank(potIndex, value, overrides) {
    const config = potConfig[POT_BANKS.ABLETON];
    const channel = overrides.channel ?? config.channel;
    const shiftHeld = overrides.shiftHeld ?? potState.shiftHeld;
    const nextValue = nextPotValue(POT_BANKS.ABLETON, potIndex, value);
    const isRelative = (config.encoderMode ?? "absolute") === "relative";
    const outgoingValue = isRelative ? value : nextValue;

    const volumes = config.volumeCcs ?? [];
    const sends = config.sendACcs ?? [];

    if (potIndex < volumes.length && potIndex < sends.length) {
        const ccOut = shiftHeld ? sends[potIndex] : volumes[potIndex];
        sendCc(channel, ccOut, outgoingValue, config.internalEcho);
        return {
            handled: true,
            bank: POT_BANKS.ABLETON,
            potIndex,
            cc: ccOut,
            value: nextValue,
            sentValue: outgoingValue,
            channel,
        };
    }

    const masterIndex = config.potCount - 1;
    if (potIndex === masterIndex) {
        const ccOut = shiftHeld ? config.returnCc : config.masterCc;
        sendCc(channel, ccOut, outgoingValue, config.internalEcho);
        return {
            handled: true,
            bank: POT_BANKS.ABLETON,
            potIndex,
            cc: ccOut,
            value: nextValue,
            sentValue: outgoingValue,
            channel,
        };
    }

    return false;
}

export function handleMoveKnobs(data, options = {}) {
    if (!(data[0] === 0xb0)) {
        return false;
    }

    const activeBank = options.activeBank ?? potState.activeBank;
    const selectedTrackIndex = options.selectedTrackIndex ?? potState.selectedTrackIndex;
    const trackScopedOverride = options.trackScopedPots;
    const shiftOverride = options.shiftHeld ?? potState.shiftHeld;

    const config = potConfig[activeBank] ?? potConfig[POT_BANKS.M8_TRACK];
    const ccStart = config.ccBase ?? 71;
    const potCount = config.potCount ?? 9;

    const moveControlNumber = data[1];
    const value = data[2];

    if (moveControlNumber < ccStart || moveControlNumber >= ccStart + potCount) {
        return false;
    }

    const potIndex = moveControlNumber - ccStart;

    if (activeBank === POT_BANKS.M8_TRACK) {
        return handleTrackBank(potIndex, value, { selectedTrackIndex, trackScoped: trackScopedOverride });
    }
    if (activeBank === POT_BANKS.M8_MASTER) {
        return handleMasterBank(potIndex, value, options);
    }
    if (activeBank === POT_BANKS.M8_FX) {
        return handleFxBank(potIndex, value, options);
    }
    if (activeBank === POT_BANKS.ABLETON) {
        return handleAbletonBank(potIndex, value, { shiftHeld: shiftOverride });
    }

    return false;
}
