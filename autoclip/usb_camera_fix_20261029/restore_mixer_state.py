"""Restore the visible mixer selection after a brief service restart."""
import json
import sys
import time

from command_ipc import CommandSender, MIXER_COMMAND_SOCKET

with open(sys.argv[1]) as state_file:
    state = json.load(state_file)

sender = CommandSender(MIXER_COMMAND_SOCKET, 'mixer')
fields = ('ch_a', 'ch_b', 'mix', 'luma_high', 'luma', 'luma_src',
          'luma_high_enabled', 'luma_feather')
for field in fields:
    action = 'luma_high_en' if field == 'luma_high_enabled' else field
    sender.send({'action': action, 'value': state[field]})

deadline = time.monotonic() + 8
while sender.delivered < len(fields) and time.monotonic() < deadline:
    time.sleep(0.05)
delivered = sender.delivered
sender.close()
if delivered != len(fields):
    raise RuntimeError('restored only {} of {} mixer values'.format(delivered, len(fields)))
