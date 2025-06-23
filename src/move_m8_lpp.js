// import { aftertouchToModwheel } from "./aftertouch_to_modwheel.mjs";
import { handleMoveKnobs } from "./move_virtual_knobs.mjs";
import { clamp } from "./math_helpers.mjs";

// https://github.com/Ableton/push-interface/blob/main/doc/AbletonPush2MIDIDisplayInterface.asc#setting-led-colors

// https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/downloads/LPP3_prog_ref_guide_200415.pdf

const moveControlToLppNoteMap = new Map([
    [55, 80],
    [54, 70],
    [62, 91],
    [63, 92],
    [85, 20],
    [43, 89],
    [42, 79],
    [41, 69],
    [40, 59],
    [50, 94],
    [49, 90],
    [119, 60],
    [51, 93],
    [52, 97],
    [88, 2],
    [56, 1],
    [86, 10],
    [60, 50],
    [58, 3],
    [118, 98]
    // [78, 99]
]);


const lppNoteToMoveControlMap = new Map([...moveControlToLppNoteMap.entries()].map((a) => [a[1], a[0]]));

const lppPadToMovePadMap = new Map([
    [81, 92], [82, 93], [83, 94], [84, 95], [85, 96], [86, 97], [87, 98], [88, 99],
    [71, 84], [72, 85], [73, 86], [74, 87], [75, 88], [76, 89], [77, 90], [78, 91],
    [61, 76], [62, 77], [63, 78], [64, 79], [65, 80], [66, 81], [67, 82], [68, 83],
    [51, 68], [52, 69], [53, 70], [54, 71], [55, 72], [56, 73], [57, 74], [58, 75],
    [101, 16], [102, 18], [103, 20], [104, 22], [105, 24], [106, 26], [107, 28], [108, 30],

])

const moveToLppPadMap = new Map([...lppPadToMovePadMap.entries()].map((a) => [a[1], a[0]]));

const light_grey = 0x7c;
const green = 0x7e;
const red = 0x7f;
const blue = 0x5f;
const white = 0xfa;
const pink = 0x6d;
const cyan = 0x5d;
const black = 0x00;

const moveMENU = 50;
const moveBACK = 51;
const moveSHIFT = 49;
const moveWHEEL = 3;
const movePLAY = 85;
const moveREC = 86;
const moveLOOP = 58;
const moveMUTE = 88;
const moveTRACK1 = 16;

const lppColorToMoveColorMap = new Map([
    [0x15, green], [0x17, light_grey], [0x1, light_grey], [0x05, red], [0x03, white], [0x4e, blue],
    [0x47, pink], [0x13, cyan]
]);

const moveColorToLppColorMap = new Map([...lppColorToMoveColorMap.entries()].map((a) => [a[1], a[0]]));

const lppColorToMoveMonoMap = new Map([
    [0x05, 0x7f], [0x78, 0x7f], [0x01, 0x10]
]);

const initDone = 1000;
const stepDelay = 20;
let shiftHeld = false;
let initStep = 0;
let timeStart = new Date();

globalThis.onMidiMessageExternal = function (data) {
    if (data[0] == 0xf8) {
        // midi clock, ignoring...
        return;
    }

    console.log(`onMidiMessageExternal ${data[0].toString(16)} ${data[1].toString(16)} ${data[2].toString(16)}`);

    let value = data[0];
    let maskedValue = (value & 0xf0);

    let noteOn = maskedValue === 0x90;
    let noteOff = maskedValue === 0x80;

    // console.log(value, maskedValue, noteOn, noteOff);

    if (!(noteOn || noteOff)) {
        console.log(`Got message from M8 that is not a note: ${data}`);
    }

    let lppNoteNumber = data[1];
    let lppVelocity = data[2];

    let moveNoteNumber = lppPadToMovePadMap.get(lppNoteNumber);
    let moveVelocity = lppColorToMoveColorMap.get(lppVelocity) ?? lppVelocity;


    if (moveNoteNumber) {
        console.log(`Got note on value : ${value}`);
        move_midi_internal_send([0 << 4 | (maskedValue / 16), maskedValue, moveNoteNumber, moveVelocity]);
        if (value === 0x92) {
            move_midi_internal_send([0 << 4 | (value / 16), 0x9a, moveNoteNumber, light_grey]);
        }
        return;
    }

    let moveControlNumber = lppNoteToMoveControlMap.get(lppNoteNumber);

    // hack for white LED controls
    if (moveControlNumber === moveLOOP || moveControlNumber === moveMUTE) {
        moveVelocity = lppColorToMoveMonoMap.get(lppVelocity) ?? lppVelocity;
    }

    if (moveControlNumber) {
        console.log(`Sending LPP note ${lppNoteNumber} velocity ${lppVelocity} to Move control ${moveControlNumber} value: ${moveVelocity}`);

        move_midi_internal_send([0 << 4 | 0xb, 0xB0, moveControlNumber, moveVelocity]);
        if (value === 0x91) {
            move_midi_internal_send([0 << 4 | 0xb, 0xbd, moveControlNumber, black]);
        }
        return;
    }

    console.log(`Unmapped LPP note: ${lppNoteNumber}`);

}

globalThis.onMidiMessageInternal = function (data) {

    let isNote = data[0] === 0x80 || data[0] === 0x90;
    let isCC = data[0] === 0xb0;
    let isAt = data[0] === 0xa0;

    if (isAt) {
        // ignore aftertouch for now
        return;
    }

    console.log(`onMidiMessageInternal ${data[0].toString(16)} ${data[1].toString(16)} ${data[2].toString(16)}`);
    if (!(isNote || isCC)) {
        console.log(`Move: unknown message:`, data);
        return;
    }

    if (isNote) {
        let moveNoteNumber = data[1];
        let lppNote = moveToLppPadMap.get(moveNoteNumber);

        if (!lppNote) {
            console.log(`Move: unmapped note [${moveNoteNumber}]`);
            return;
        }

        let moveVelocity = data[2] * 2;

        moveVelocity = moveVelocity * 2;
        if (moveVelocity > 127) {
            moveVelocity = 127;
        }


        let lppVelocity = moveVelocity; //moveVelocityToLppVelocityMap.get(data[2]) ?? moveVelocity;


        console.log(`Sending Move note ${moveNoteNumber} velocity: ${moveVelocity} to LPP pad ${lppNote} velocity: ${moveVelocity}`);
        move_midi_external_send([2 << 4 | (data[0] / 0xF), data[0], lppNote, moveVelocity]);
        return;
    }

    if (isCC) {

        console.log("control message");
        let moveControlNumber = data[1];

        let lppNote = moveControlToLppNoteMap.get(moveControlNumber);

        // if Shift is held, exit if Wheel is pressed
        if (shiftHeld == true && moveControlNumber === moveWHEEL) {
            console.log ("Shift+Wheel - exit");
            exit();
            return;
        }


        if (!lppNote) {
            console.log(`Move: unmapped control [${moveControlNumber}]`);
            handleMoveKnobs(data);
            return;
        }

        let pressed = data[2] === 127;

        console.log(`Sending Move control ${moveControlNumber} to LPP pad ${lppNote} pressed:${pressed}`);
        if (pressed) {
            // detect and store Shift button state
            if (moveControlNumber === 49) {
                shiftHeld = true;
                console.log ("Shift Held");
            }

            move_midi_external_send([2 << 4 | 0x9, 0x90, lppNote, 100]);

        } else {
            // reset Shift state when released
            if (moveControlNumber === 49) {
                console.log ("Shift Released");
                shiftHeld = false;
            }
            
            move_midi_external_send([2 << 4 | 0x8, 0x80, lppNote, 0]);
        }
        return;
    }

    // if (aftertouchToModwheel(data)) {
    //     return;
    // }

    console.log(`Unmapped Move message: ${data}`);

}

// let lppInitSysex = [0xF0, 126, 0, 6, 2, 0, 32, 41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7];

function initLPP() {
    let out_cable = 2;
    let LPPInitSysex = [
        out_cable << 4 | 0x4, 0xF0, 126, 0,
        out_cable << 4 | 0x4, 6, 2, 0,
        out_cable << 4 | 0x4, 32, 41, 0x00,
        out_cable << 4 | 0x4, 0x00, 0x00, 0x00,
        out_cable << 4 | 0x4, 0x00, 0x00, 0x00,
        out_cable << 4 | 0x6, 0x00, 0xF7, 0x0];


    // move_midi_external_send(lppInitSysex);

    // Trigger LPP mode on the M8
    console.log("Sending M8 LPP init");
    move_midi_external_send(LPPInitSysex);
}

function initMove(step) {
    // go to NOTE view and then back to SONG to initialise PADS. 
    // ensure SHIFT is not set as pressed
    // toggle MUTE->SOLO to setup track lights under bottom row of matching PADS
    // set PLAY and REC dim LEDS
    // add any additional steps with increased case #, ensuring last case has return initDone
    // steps are taken at stepDelay intervals - cases do not need to be consective, just multiplies of stepDelay as required

    let timeStamp = new Date();
    let duration = timeStamp.getTime() - timeStart.getTime();

    if (duration > step * stepDelay && duration < (step + 1) * stepDelay) {
        switch (step) {
            case 2: move_midi_internal_send([0 << 4 | 0xb, 0xB0, movePLAY, light_grey]); break;
            case 3: move_midi_internal_send([0 << 4 | 0xb, 0xB0, moveREC, light_grey]); break;
            case 4: move_midi_external_send([2 << 4 | 0x9, 0x90, moveControlToLppNoteMap.get(moveMENU), 100]); break;
            case 5: move_midi_external_send([2 << 4 | 0x9, 0x90, moveControlToLppNoteMap.get(moveMENU), 0]); break;
            case 6: move_midi_external_send([2 << 4 | 0x9, 0x90, moveControlToLppNoteMap.get(moveBACK), 100]); break;
            case 7: move_midi_external_send([2 << 4 | 0x9, 0x90, moveControlToLppNoteMap.get(moveBACK), 0]); break;
            case 8: move_midi_external_send([2 << 4 | 0x9, 0x90, moveControlToLppNoteMap.get(moveSHIFT), 100]); break;
            case 9: move_midi_external_send([2 << 4 | 0x9, 0x90, moveControlToLppNoteMap.get(moveSHIFT), 0]); break;
            case 10: move_midi_external_send([2 << 4 | 0x9, 0x90, moveControlToLppNoteMap.get(moveMUTE), 100]); break;
            case 11: move_midi_external_send([2 << 4 | 0x9, 0x90, moveControlToLppNoteMap.get(moveMUTE), 0]); break;
            case 12: move_midi_external_send([2 << 4 | 0x9, 0x90, moveControlToLppNoteMap.get(moveLOOP), 100]); break;
            case 13: move_midi_external_send([2 << 4 | 0x9, 0x90, moveControlToLppNoteMap.get(moveLOOP), 0]); break;
            case 14: move_midi_external_send([2 << 4 | 0x9, 0x90, moveToLppPadMap.get(moveTRACK1), 127]); break;
            case 15: move_midi_external_send([2 << 4 | 0x9, 0x90, moveToLppPadMap.get(moveTRACK1), 0]); break;
            case 16: move_midi_external_send([2 << 4 | 0x9, 0x90, moveToLppPadMap.get(moveTRACK1), 127]); break;
            case 17: move_midi_external_send([2 << 4 | 0x9, 0x90, moveToLppPadMap.get(moveTRACK1), 0]); return initDone;
        }
        return step + 1;
    } else {
        return step;
    }
}

globalThis.init = function () {
    console.log("Move control surface script staring...");
    initLPP();
    // console.log("Calling exit...");
    // exit();
}

globalThis.tick = function() {
    if (initStep != initDone) {
        initStep = initMove(initStep);
    }
};
