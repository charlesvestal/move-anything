/*
 * AbleM8 — Ableton/YURS profile configuration
 *
 * The note and CC assignments in yursProfile are derived from the YURS
 * (Yaeltex Universal Remote Script) default mapping for 8-track control.
 * Source: https://forum.yaeltex.com/t/yurs-yaeltex-universal-remote-script-for-ableton-live/161
 *
 * If your YURS installation uses a different channel or CC/note layout,
 * edit the values below to match. Everything else in the module adapts
 * automatically at runtime via configurePotBank().
 */

export const ablem8Config = {
    // CC/note map for Ableton/YURS bank.
    // Channel 16 (index 15) is the YURS default control channel.
    yursProfile: {
        midiChannel: 15,
        encoderMode: "relative",
        // Source: [YURS] — default note assignments for track mute/solo/arm feedback.
        notes: {
            mute: [0, 1, 2, 3, 4, 5, 6, 7],
            solo: [16, 17, 18, 19, 20, 21, 22, 23],
            arm: [32, 33, 34, 35, 36, 37, 38, 39]
        },
        // Source: [YURS] — default CC assignments for volume, sends, master, and macros.
        ccs: {
            volume: [0, 1, 2, 3, 4, 5, 6, 7],
            sendA: [16, 17, 18, 19, 20, 21, 22, 23],
            masterVolume: 127,
            returnVolume: 117,
            macros: [32, 33, 34, 35, 36, 37, 38, 39]
        },
        feedback: {
            noteOn: 127,
            noteOff: 0,
            expectsNotes: true,
            expectsCcs: true
        }
    },
    clipWarningThreshold: 118
};
