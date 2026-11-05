"""Briefly select EXT USB, then restore the mixer values present at test time."""
import json
import time

from command_ipc import CommandSender, MIXER_COMMAND_SOCKET

STATE_FILE = '/tmp/mixer_state.json'
with open(STATE_FILE) as source:
    previous = json.load(source)

sender = CommandSender(MIXER_COMMAND_SOCKET, 'mixer')

def send_all(commands):
    start = sender.delivered
    for action, value in commands:
        sender.send({'action': action, 'value': value})
    target = start + len(commands)
    deadline = time.monotonic() + 8
    while sender.delivered < target and time.monotonic() < deadline:
        time.sleep(0.05)
    if sender.delivered != target:
        raise RuntimeError('mixer command delivery incomplete')

try:
    send_all([('luma_src', 0), ('mix', 0), ('ch_a', 4)])
    time.sleep(5)
finally:
    restore = [('ch_a', previous['ch_a']), ('ch_b', previous['ch_b']),
               ('mix', previous['mix']), ('luma_high', previous['luma_high']),
               ('luma', previous['luma']), ('luma_src', previous['luma_src']),
               ('luma_high_en', previous['luma_high_enabled']),
               ('luma_feather', previous['luma_feather'])]
    send_all(restore)
    sender.close()
    print('Restored mixer values:', json.dumps(previous, sort_keys=True))
