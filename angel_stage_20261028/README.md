# ANGEL source — 2026-10-28

ANGEL is source 28 on autoclip. It is selectable independently for A, B, and
LUMA (LUMA menu index 29). The geometry shader draws paired upper and lower
feather fans around a multi-eye core on black. The drawing is generated in
GLSL, with no image assets required.

The side keypad's three controls for each source role are WING MOTION, WING
SPREAD, and EYE AWAKEN. Each has the existing base plus low/mid/high audio
reactivity depths. MOTION also controls the phase clock speed. The shader
renders internally at 360×288 and scales to the 720×576 mixer surface.

On the Teensy Processing screen, scroll to ANGEL and press or hold its encoder
button to open the shape page. The first row is < BACK; press it to return to
Processing. Turn to choose one of 12 controls, press to edit its value, and
press again to choose another control. Upper and lower wings each have span, feather count, curve,
and flutter. The core has body scale, eye count, halo count, and palette.

Shape values are saved immediately in /home/admin/angel_shape.json and
queried when opening the page. They are global across video patches. Audio
patches still store the three per-role macro controls and their band depths.

Live deployment files are in live_autoclip/; controller source and patch
schema are in auto_midi_v0911/; the renderer schema is
renderer_video_patch_schema.h. Rollback files on the devices are
/home/admin/angel_backup_20261028/ on autoclip,
src/video_patch_schema.h.angel_backup_20261028 and
/home/admin/auto_midi_pre_angel_20261028.hex on autowaaave.
