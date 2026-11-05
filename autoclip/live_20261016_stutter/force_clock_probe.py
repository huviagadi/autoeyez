"""Read-only probe for the Force's clock and beat-repeat CC values."""

import statistics
import time

import mido


name = next(port for port in mido.get_input_names() if 'f_midi:' in port)
clock_times = []
controls = {22: [], 24: []}
transport = []
with mido.open_input(name) as port:
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        for message in port.iter_pending():
            now = time.monotonic()
            if message.type == 'clock':
                clock_times.append(now)
            elif message.type in ('start', 'continue', 'stop'):
                transport.append(message.type)
            elif message.type == 'control_change' and message.control in controls:
                controls[message.control].append(message.value)
        time.sleep(0.002)

intervals = [b - a for a, b in zip(clock_times, clock_times[1:])]
plausible = [value for value in intervals if 0.005 < value < 0.1]
print('clock_ticks', len(clock_times))
print('estimated_bpm', round(60 / (24 * statistics.median(plausible)), 2)
      if plausible else None)
print('transport', transport)
for cc, values in controls.items():
    print('cc', cc, 'count', len(values), 'range',
          (min(values), max(values)) if values else None,
          'sample', values[:20])
