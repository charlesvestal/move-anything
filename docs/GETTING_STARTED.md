# Getting Started: Display “HELLO WORLD”

This quick tour shows how to draw text on the Ableton Move display using the QuickJS helpers exposed by `control_surface_move`. The native layer already handles pushing pixel data to the hardware; the script only needs to set pixels and text.

## 1. Duplicate the Default Script
Copy the baseline control surface script so you keep the expected handler signatures:
```bash
cp src/move_default.js src/hello_world_display.js
```

## 2. Replace the Script Contents
Open `src/hello_world_display.js` and replace its body with the snippet below. The C runtime exports `clear_screen`, `print`, and other drawing helpers, so there is no `os.mmap` or manual flush step.

```js
globalThis.init = function () {
  clear_screen();          // wipe the framebuffer
  print(24, 18, "HELLO", 1);
  print(20, 36, "WORLD", 1);
};

globalThis.tick = function () {
  // Optional: Update animation or UI every loop. Left empty here.
};

globalThis.onMidiMessageInternal = function (_msg) {};

globalThis.onMidiMessageExternal = function (_msg) {};
```

### Drawing Helpers Available
- `set_pixel(x, y, value)` – set a single pixel (value 1 = white, 0 = off).
- `draw_rect(x, y, width, height, value)` – outline rectangle.
- `fill_rect(x, y, width, height, value)` – filled rectangle.
- `clear_screen()` – blank the entire 128×64 buffer.
- `print(x, y, text, value)` – draw text using the bundled bitmap font (value 1 = white).
The C loop automatically detects changes and pushes the updated framebuffer to the Move’s OLED.

## 3. Build & Package
Run the standard pipeline so your new script is included in the deployable bundle:
```bash
./build.sh
./package.sh
```
Generated assets appear under `dist/control_surface_move/` and the `control_surface_move.tar.gz` archive.

## 4. Deploy to the Move
Copy the bundle and shim to the device:
```bash
./copy_to_move.sh
```
This stops the stock Move binaries, uploads the artifacts to `/data/UserData/control_surface_move/`, and restores the SUID shim so the runtime can access the SPI mailbox.

## 5. Launch the Script
Ensure the factory processes are stopped (the helper script does this for you):
```bash
ssh ableton@move.local "killall MoveLauncher Move MoveOriginal MoveMessageDisplay"
ssh ableton@move.local "cd control_surface_move && ./control_surface_move ./hello_world_display.js"
```
You should see “HELLO WORLD” on the display. If you prefer to boot straight into this demo, edit `/data/UserData/control_surface_move/start_control_surface_move.sh` to point at `hello_world_display.js`.

## 6. Troubleshooting
- If nothing appears, confirm the script ran without exceptions (`ssh` session will print stack traces). Syntax errors in `init` prevent the helper from drawing.
- Still seeing the stock UI? Make sure the `killall` command ran before launching your script, and that `/usr/lib/control_surface_move_shim.so` exists with SUID (`ls -l` should show `-rwsr-xr-x`).
- To experiment with graphics, call `draw_rect` or `set_pixel` inside `tick()`; the display will refresh automatically whenever you call the helpers.
