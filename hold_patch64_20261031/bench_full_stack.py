#!/usr/bin/env python3
"""Brief live A/B of safe versus fused chroma/crush, then restore controls."""
import pathlib
import re
import shutil
import subprocess
import sys
import time

import mido


PROFILE = pathlib.Path('/dev/shm/waaave_optimization_mode')
CAPTURE = pathlib.Path('/dev/shm/waaave_capture_once')
SCREENSHOT = pathlib.Path('/tmp/waaave_output.png')
LOG = pathlib.Path('/tmp/pi_5_stable_04.log')
VALUES = {95: 35, 96: 40, 97: 2, 107: 127, 108: 127}
FIRST_MODE = int(sys.argv[1]) if len(sys.argv) > 1 else 0
SECOND_MODE = int(sys.argv[2]) if len(sys.argv) > 2 else 4


def client_id(name):
    listing = subprocess.check_output(['aconnect', '-l'], text=True)
    match = re.search(r"client (\d+): '" + re.escape(name) + "'", listing)
    if not match:
        raise RuntimeError(f'ALSA MIDI client missing: {name}')
    return int(match.group(1))


def recent_perf(label):
    lines = LOG.read_text(errors='replace').splitlines()
    for line in [line for line in lines if 'STAGE_PERF' in line][-3:]:
        print(label, line, flush=True)


original = PROFILE.read_bytes() if PROFILE.exists() else None
source = None
target = None
port = mido.open_output('CodexHOLDTest', virtual=True)
try:
    source = client_id('RtMidiOut Client')
    target = client_id('ofxMidiIn Client')
    subprocess.run(['aconnect', f'{source}:0', f'{target}:0'], check=True)
    PROFILE.write_text(f'{FIRST_MODE}\n')
    for control, value in VALUES.items():
        port.send(mido.Message('control_change', channel=0,
                               control=control, value=value))
    time.sleep(12)
    CAPTURE.touch()
    time.sleep(2)
    shutil.copy2(SCREENSHOT, f'/tmp/hold_stack_mode{FIRST_MODE}.png')
    recent_perf(f'MODE{FIRST_MODE}')

    PROFILE.write_text(f'{SECOND_MODE}\n')
    time.sleep(15)
    if SECOND_MODE & 16:
        pathlib.Path('/dev/shm/waaave_verify_direct').touch()
        time.sleep(2)
    CAPTURE.touch()
    time.sleep(2)
    shutil.copy2(SCREENSHOT, f'/tmp/hold_stack_mode{SECOND_MODE}.png')
    recent_perf(f'MODE{SECOND_MODE}')
    if SECOND_MODE & 16:
        lines = LOG.read_text(errors='replace').splitlines()
        for line in [line for line in lines if 'DIRECT_VERIFY' in line][-2:]:
            print(line, flush=True)
finally:
    for control in VALUES:
        port.send(mido.Message('control_change', channel=0,
                               control=control, value=0))
    if original is None:
        PROFILE.unlink(missing_ok=True)
    else:
        PROFILE.write_bytes(original)
    if source is not None and target is not None:
        subprocess.run(['aconnect', '-d', f'{source}:0', f'{target}:0'],
                       check=False)
    port.close()
    print('RESTORED controls and temporary render profile', flush=True)
