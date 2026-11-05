# Audio/video reset update, 2026-11-04

The controller's audio/video reset now clears controller encoder values and toggles, all base and band-specific Processing values, Audio Codec and Static Codec masters, and the current macro pad shader offset and reactivity patch. Saved audio patch slots remain available. It routes channel A to CLIPS, sets MIX to 0 (A only), turns off both luma cuts, and restores the low/high thresholds to 0/127 with luma source NONE. The macro pad's current shader patch is reset to neutral 64 offsets/reactivity and republished to the mixer.

The changed firmware compiled for Teensy 4.1 and was flashed to autowaaave. The updated `video_control.py` and `side_keypad.py` compiled and their services restarted active. Existing Codec values were restored after the firmware flash. The live mixer reported 30 FPS with no over-budget frames. A physical reset-button press has not been exercised during deployment to avoid changing the current performance state.
