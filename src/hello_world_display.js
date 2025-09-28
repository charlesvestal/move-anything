globalThis.init = function () {
  clear_screen();
  print(24, 18, "HELLO", 1);
  print(20, 36, "WORLD", 1);
};

globalThis.tick = function () {};

globalThis.onMidiMessageInternal = function (_msg) {};

globalThis.onMidiMessageExternal = function (_msg) {};
