globalThis.init = function () {
  clear_screen();
  print(24, 18, "HELLO", 1);
  print(20, 36, "WORLD", 1);
};

globalThis.tick = function () {};

let volumeTouched = false;
let wheelPressed = false;

function handleInternalMidi(data) {
  const status = data[0];
  const control = data[1];
  const value = data[2];

  const isNote = status === 0x90 || status === 0x80;

  if (!isNote) {
    return;
  }

  if (control === 0x08) {
    volumeTouched = status === 0x90 && value === 0x7f;
    if (status === 0x80 || value === 0x00) {
      volumeTouched = false;
    }
  }

  if (control === 0x09) {
    wheelPressed = status === 0x90 && value === 0x7f;
    if (status === 0x80 || value === 0x00) {
      wheelPressed = false;
    }
  }

  if (volumeTouched && wheelPressed) {
    exit();
  }
}

globalThis.onMidiMessageInternal = function (data) {
  handleInternalMidi(data);
};

globalThis.onMidiMessageExternal = function (_msg) {};
