#!/usr/bin/env python3
"""Recover a configured g_midi endpoint whose active Rx counter freezes."""

import glob
import re
import subprocess
import time

POLL_SECONDS = 2
STALL_SECONDS = 20
STALL_POLLS = STALL_SECONDS // POLL_SECONDS


def rx_bytes():
    for path in glob.glob('/proc/asound/card*/midi*'):
        try:
            text = open(path).read()
        except OSError:
            continue
        match = re.search(r'Rx bytes\s*:\s*(\d+)', text)
        if match:
            return int(match.group(1))
    return None


def recover():
    print('MIDI Rx frozen for {}s; reloading g_midi'.format(STALL_SECONDS),
          flush=True)
    subprocess.run(['systemctl', 'stop', 'force_cc_forward.service'], check=False)
    subprocess.run(['modprobe', '-r', 'g_midi'], check=False)
    time.sleep(1)
    subprocess.run(['modprobe', 'g_midi', 'qlen=1024', 'buflen=512'], check=True)
    time.sleep(2)
    subprocess.run(['systemctl', 'restart', 'force_cc_forward.service'], check=False)


def main():
    previous = None
    stagnant = 0
    while True:
        current = rx_bytes()
        # After recovery the counter is zero until the Force reconnects. Do not
        # loop on a disconnected or genuinely absent host.
        if current is None or current == 0:
            previous = current
            stagnant = 0
        elif current == previous:
            stagnant += 1
            if stagnant >= STALL_POLLS:
                recover()
                previous = None
                stagnant = 0
        else:
            previous = current
            stagnant = 0
        time.sleep(POLL_SECONDS)


if __name__ == '__main__':
    main()
