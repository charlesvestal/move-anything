// AbleM8 mapping config (YURS-derived). YURS reference:
// https://forum.yaeltex.com/t/yurs-yaeltex-universal-remote-script-for-ableton-live/161

export const ablem8Config = {
    // CC/note map for Ableton/YURS bank
    yursProfile: {
        midiChannel: 15,
        encoderMode: "relative",
        notes: {
            mute: [0, 1, 2, 3, 4, 5, 6, 7],
            solo: [16, 17, 18, 19, 20, 21, 22, 23],
            arm: [32, 33, 34, 35, 36, 37, 38, 39]
        },
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
