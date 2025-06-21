import { aftertouchToModwheel } from "./aftertouch_to_modwheel.mjs";
import { handleMoveKnobs } from "./move_virtual_knobs.mjs";
import { clamp } from "./math_helpers.mjs";

/*
Notes: 
Sequencer 16 - 31
Grid 68-99 (Bottom left to top right)
Sequencer steps: 16-31
Track selectors 40-43

CCs:
UI items below sequencer steps: 16-31
Shift 49
Menu 50
Capture 52
Down 54
Up 55
Undo 56
Loop 58
Copy 60
Left 62
Right 63
Knob Indicators 71-78
Play 85
Rec 86
Mute 88
Record (audio) 118
Delete 119
*/

globalThis.onMidiMessageExternal = function (data) {
    console.log(`onMidiMessageExternal ${data[0].toString(16)} ${data[1].toString(16)} ${data[2].toString(16)}`);

    move_midi_internal_send([0 << 4 | (data[0] / 16), data[0], data[1], data[2]]);
}

globalThis.onMidiMessageInternal = function (data) {
    console.log(`onMidiMessageInternal ${data[0].toString(16)} ${data[1].toString(16)} ${data[2].toString(16)}`);


    if (handleMoveKnobs(data)) {
        return;
    }

    if (aftertouchToModwheel(data)) {
        return;
    }

    move_midi_external_send([2 << 4 | (data[0] / 16), data[0], data[1], data[2]]);
}

/*
        https://www.usb.org/sites/default/files/midi10.pdf

        0x5     1               Single-byte System Common Message or SysEx ends with following single byte.
        0x6     2               SysEx ends with following two bytes.
        0x7     3               SysEx ends with following three bytes.
*/

// Example: [F0 00 21 1D 01 01 05 F7] = trigger palette reapplication

// Example: [F0 00 21 1D 01 01 03 7D 00 00 00 00 7F 01 7E 00 F7] = set entry 125 to 0/0/255 and 126

function setPaletteEntry(palette_index, r, g, b, w) {
    console.log(`setPaletteEntry: index:${palette_index} (${r},${g},${b},${w})`);
    let sysex_start = 0xF0;
    let sysex_end = 0xF7;
    let ableton_sysex_id = [0x00, 0x21, 0x1D]
    let device_id = 0x01;
    let model_id = 0x01;
    let palette_command_id = 0x03;

    r = clamp(r, 0, 255);
    g = clamp(g, 0, 255);
    b = clamp(b, 0, 255);
    w = clamp(w, 0, 255);
    palette_index = clamp(palette_index, 0, 127);

    let red_low = r & 0x7f; // bottom 7 bits
    let red_high = r >> 7; // top 1 bit

    let green_low = g & 0x7f; // bottom 7 bits
    let green_high = g >> 7; // top 1 bit

    let blue_low = b & 0x7f; // bottom 7 bits
    let blue_high = b >> 7; // top 1 bit

    let white_low = w & 0x7f; // bottom 7 bits
    let white_high = w >> 7; // top 1 bit

    let palette_sysex = [
        sysex_start,
        ...ableton_sysex_id,
        device_id, model_id,
        palette_command_id, 
        palette_index,
        red_low, red_high,
        green_low, green_high,
        blue_low, blue_high,
        white_low, white_high,
        sysex_end // end sysex
    ]

    sendSysexInternal(palette_sysex);
}

let INTERNAL_CABLE = 0;
function sendSysexInternal(data) {
    let output = [];
    const cable_number = INTERNAL_CABLE; // This is 0

    // Iterate through the data in chunks of 3 bytes
    for (let i = 0; i < data.length; i += 3) {
        const remainingBytes = data.length - i;
        let cin;
        let byte1 = data[i];
        let byte2 = 0x00;
        let byte3 = 0x00;

        // Determine the correct Code Index Number (CIN)
        if (remainingBytes >= 3) {
            byte2 = data[i + 1];
            byte3 = data[i + 2];
            // If this is the last chunk of exactly 3 bytes, it's a "SysEx ends with 3" packet
            cin = (remainingBytes === 3) ? 0x7 : 0x4;
        } else if (remainingBytes === 2) {
            byte2 = data[i + 1];
            // Last chunk of 2 bytes is a "SysEx ends with 2" packet
            cin = 0x6;
        } else { // remainingBytes === 1
            // Last chunk of 1 byte is a "SysEx ends with 1" packet
            cin = 0x5;
        }

        output.push(
            (cable_number << 4) | cin,
            byte1,
            byte2,
            byte3
        );
    }

    for (let i = 0; i < output.length; i += 4) {
        console.log(
            output[i].toString(16),
            output[i + 1].toString(16),
            output[i + 2].toString(16),
            output[i + 3].toString(16),
        );
    }

    console.log("");
    // console.log(output);

    move_midi_internal_send(output);
}

function reapplyPalette() {
    let palette_reapplication_sysex = [0xF0, 0x00, 0x21, 0x1D, 0x01, 0x01, 0x05, 0xF7];

    sendSysexInternal(palette_reapplication_sysex);
}

// [F0 00 21 1D 01 01 03 7D 00 00 00 00 7F 01 7E 00 F7] = set entry 125 to 0/0/255 and 126
function testPalette() {

    // for (let i = 0; i < 128; i++) {
    //     setPaletteEntry(i, i * 32, 0, 0, 0);
    // }
    
    // reapplyPalette();
    setPaletteEntry(0, 6, 0, 6, 32);

    reapplyPalette();

    // sendSysexInternal([0xF0,0x00,0x21,0x1D,0x01,0x01,0x06,0x20,0xF7]);
}


globalThis.init = function () {
    console.log("Move default control surface script starting...");

    testPalette();


    for (let i = 0; i < 32; i++) {
        // console.log(`set pad:${i} ${(i+68) == 87 ? ">>>>>>>>>>>>>>>>>" : ""}\n`);
        move_midi_internal_send([0 << 4 | 0x9, 0x90, i + 68, 0]);
        // move_midi_internal_send([0<<4 | 0x9, 0x9a, i + 68, 0]);        
    }
}

let animCounter = 0;
let index = 0;
globalThis.tick = function(deltaTime) {

    if (animCounter++ < 8) {
        return;
    }
    
    setPaletteEntry(0, index++ % 255, 0, 0, 32);
    reapplyPalette();

    // console.log(animCounter);

    
//     move_midi_internal_send([0<<4 | 0xf, 0xf8, 0, 0]);

    animCounter = 0;
}