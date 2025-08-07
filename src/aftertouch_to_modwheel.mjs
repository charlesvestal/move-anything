export function aftertouchToModwheel(data, channel = 3) {
    if (!(data[0] === 0xa0)) {
        return false;
    }

    let value = data[2];
    // Send per note aftertouch out as a single MIDI Modwheel CC
    console.log(
        `Sending Move aftertouch value ${value} as CC 1 on channel ${channel}`
    );

    move_midi_external_send([(2 << 4) | 0xb, 0xb0 | channel, 1, value]);

    return true;
}
