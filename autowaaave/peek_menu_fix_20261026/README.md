# Peek Processing menu fix — deployed 2026-10-26

The controller's page 2 Processing list now ends with **PEEK** and **PEEK MASK**, after HOLD. Scroll with encoder 7, press to edit, and turn it. PEEK uses CC108: in normal mode it sets effect strength; with LOW, MID, or HIGH selected it sets that band's depth. PEEK MASK uses CC109 and displays ALPHA or LUMA.

Video patches are version 5 and save PEEK strength and mask mode. Existing version 4 and version 3 video patches load with PEEK off and ALPHA mode. Audio patches are version 3 and save the three PEEK depths; existing version 2 audio patches load with neutral full depths. `patch_coordinator.py` preserves legacy cue identities when older video patches are recalled.

Files in this folder match the deployed renderer and controller source. The corresponding live copies are in the autowaaave app source tree and `/home/admin/` for the Python bridge modules. The previous live files and hex are at `/home/admin/deploy_backups/20261026_peek_menu_pre/`.

Checks: Teensy 4.1 sketch compiled; autowaaave renderer compiled; C++ and Python video schema tests passed for versions 3–5; an existing saved audio patch and a generated version 3 patch passed the audio parser test. After restart, the renderer and bridge were active, MIDI FFT returned to about 430 frames per five seconds, and render cadence remained about 30 fps. The physical OLED view was not captured remotely.
