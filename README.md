Move Control Surface
====================


What is this?
=============
1. An emulation of the Launch Pad Pro on the Ableton Move to be used with the Dirtywave M8. The Move is a USB host for the M8 and can charge it. All 9 knobs are mappable on the M8 while in LPP mode. Poly aftertouch is sent as CC1 and is also mappabe on M8.

2. A framework to develop control surface scripts in Javascript. This supports tansforming MIDI in arbitrary ways both coming from the pads, knobs and controls, and also transforming and retransmitting external midi.

3. A framework to capture keypresses while the Move's original software is running and to launch custom software, and to return to the Move software once finished.

4. As a bonus, there's an additional option to install "pages of sets" that gives you unlimited pages of sets on the Ableton Move.

Installation (macos, Linux)
===========================

1. Back up all your sets. I haven't lost any data, but I also make no guarantees that you won't!

2. Set up SSH on your move by adding an SSH key to http://move.local/development/ssh.

3. Turn on your Move and make sure it's connected to the same network as the device you're using to install.

4. The installer is currently a bash script. On macos (and probably Linux) you can just paste this into a terminal:

```bash
curl -L https://raw.githubusercontent.com/bobbydigitales/control_surface_move/main/installer/install.sh | sh
```

And it'll download the latest build and install it on your Move. 

5. The installer will ask you if you want to install "pages of sets" which is an optional bonus feature that gives you unlimited pages of sets on the move by holding shift and pressing the left or right buttons. The default is to not install. You can install later by running the installer again. _**In the current build, it will rearrange your sets and change their colors! You have been warned!**_ (I will try and fix this though)


Installation Windows
====================
If you're on Windows, you can get bash by installing Git Bash, can you get that by installing Git for Windows): https://git-scm.com/downloads/win. Once you have that, launch Bash and then run the install script as above!
   
Usage of the M8 Launchpad Pro emulation
=======================================
1. Once installed, to launch the m8 integration, hold shift then touch the volume knob and the jog wheel. Toggle Launch Pad Pro control surface mode on the M8 and the Move should come show you the session mode.

2. To see the bottom half of the Launch Pad Pro, click the wheel, the mode button you're on will flash to show you'e on the bottom half.

3. To launch Beat Repeat mode, hold shift, press session (the arrow pointing left on the left of the move), then click the wheel to show the bottom half of the Move.

4. All 9 knobs send MIDI CC's on channel 4. Poly aftertouch is mapped to CC1 on channel 4.

5. To exit M8 control surface mode and go back to Move, hold shift and click the jog wheel in.

Mapping from Launch Pad Pro to Move
===================================
<img width="3300" height="2295" alt="image" src="https://github.com/user-attachments/assets/8d94519d-0b6b-41f5-b40c-21c2b95e113f" />

Usage of "Pages of Sets"
========================
_**Note that when you change pages your live session is killed and restarted with the new page**_

1. To change pages hold shift and press the left or right arrow.
2. If a page doesn't exist, it will be created.
3. Samples, Track presets, Recordings and Effects presets are shared between all pages.
2. http://move.local will show the Sets from the current page.

After installing a new Move build
=================================
**After an update to a new Move build, you will need to re-run the install script.**

Troubleshooting
===============
If it's not working, you can get help in our Discord server: https://discord.gg/gePd9annTJ

