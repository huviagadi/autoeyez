"""Control the clean HiFiBerry-to-HDMI passthrough service."""

import subprocess


SERVICE = "alsaloop.service"


def handle_audio_pass(line):
    if line not in ("AUDIO_PASS:ON", "AUDIO_PASS:OFF"):
        print("Ignoring invalid audio pass command: {}".format(line), flush=True)
        return False
    action = "start" if line.endswith("ON") else "stop"
    try:
        result = subprocess.run(
            ["sudo", "-n", "/usr/bin/systemctl", action, SERVICE],
            capture_output=True, text=True, timeout=8, check=True,
        )
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print("HDMI audio {} failed: {}".format(action, error), flush=True)
        return False
    print("HDMI audio {} complete".format(action), flush=True)
    return True
