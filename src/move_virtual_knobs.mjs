import {clamp} from "./math_helpers.mjs"
let knobs = [0,0,0,0,0,0,0,0,0];

export function handleMoveKnobs(data) {

    if (!(data[0] === 0xb0)) {
        return false;
    }

    let knob = -1;

    let moveControlNumber = data[1];
    let value = data[2];

    console.log(moveControlNumber, value);

    // If this is a Move knob, turn it into a 0-127 value vs +/- 1 values so we can map to external devices easily.
    if (moveControlNumber >= 71 && moveControlNumber <= 80) {
        knob = moveControlNumber - 71;
    }

    if (knob != -1) {

        if (value === 127) {
            knobs[knob] -= 1;
        }

        if (value === 1) {
            knobs[knob] += 1;
        }

        knobs[knob] = clamp(knobs[knob], 0, 127);

        console.log(`Sending CC ${moveControlNumber} value: ${knobs[knob]}`);
        move_midi_external_send([2 << 4 | 0xb, 0xB3, moveControlNumber, knobs[knob]]);
        return true;
    }

    return false;
}