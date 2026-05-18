"""
video_control.py — Command/State Bridge for autoclip
Part of the autoeyez video synthesis system

This script acts as the communication hub between the automidi controller
(via autowaaave's video_bridge) and the local video player/mixer processes.

Architecture:
    automidi (Teensy)
        → serial → video_bridge.py (on autowaaave)
        → UDP:5006 → THIS SCRIPT
        → JSON files → video_player.py / mixer.py

    video_player.py / mixer.py
        → JSON state files → THIS SCRIPT
        → UDP:5005 → video_bridge.py (on autowaaave)
        → serial → automidi (Teensy)

Commands received (UDP port 5006):
    NEXT, PREV        - Navigate clips
    PLAY, PAUSE       - Playback control
    LOOP_ON, LOOP_OFF - Toggle loop mode
    PLAY:n            - Jump to clip index n
    CH_A:n, CH_B:n    - Set mixer channel sources (0-4)
    MIX:n             - Set crossfade value (0-127)
    LUMA:n            - Set luma key low threshold (0-127)
    LUMA_SRC:n        - Set luma key source (0-5)
    LUMA_HIGH:n       - Set luma key high threshold (0-127)
    LUMA_HIGH_EN:n    - Enable/disable high cut (0 or 1)

State updates sent (UDP port 5005):
    LIST:name1,name2,...  - Clip list (sent every 10s and on startup)
    FILE:name             - Current clip name
    POS:index:count       - Current clip position in list
    PAUSE:0/1             - Pause state
    LOOP:0/1              - Loop state
    PROGRESS:pos:dur      - Playback position (seconds)
"""

import socket
import os
import time
import threading
import json

# =============================================================================
# CONFIGURATION
# =============================================================================

# Network settings - autowaaave is the hub at 10.0.0.1
PI1_IP    = '10.0.0.1'   # autowaaave IP address
SEND_PORT = 5005         # Port to send state updates TO autowaaave
RECV_PORT = 5006         # Port to receive commands FROM autowaaave

# File paths
CLIPS_DIR = '/boot/firmware/clips/'   # MP4 clips stored on boot partition
STATE_FILE = '/tmp/player_state.json' # Video player writes state here
CMD_FILE   = '/tmp/player_cmd.json'   # We write commands for video player here
MIXER_STATE_FILE = '/tmp/mixer_state.json'  # Mixer writes state here (unused currently)
MIXER_CMD_FILE   = '/tmp/mixer_cmd.json'    # We write commands for mixer here

# =============================================================================
# SOCKET SETUP
# =============================================================================

# UDP socket for sending state updates to autowaaave
udp_send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# UDP socket for receiving commands from autowaaave
udp_recv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp_recv.bind(('0.0.0.0', RECV_PORT))  # Listen on all interfaces

# =============================================================================
# UTILITY FUNCTIONS
# =============================================================================

def clean_name(filename):
    """
    Convert filename to display name for OLED screens.

    Strips extension and optional numeric prefix (for sorting).
    Example: "01_coolclip.mp4" -> "COOLCLIP"

    Args:
        filename: Raw filename from clips directory

    Returns:
        Uppercase display name
    """
    name = os.path.splitext(filename)[0]  # Remove .mp4 extension
    if '_' in name:
        name = name.split('_', 1)[1]      # Remove prefix before first underscore
    return name.upper()


def get_clips():
    """
    Get sorted list of MP4 files in clips directory.

    Returns:
        List of filenames sorted alphabetically (prefix numbers ensure order)
    """
    return sorted([f for f in os.listdir(CLIPS_DIR) if f.endswith('.mp4')])


def send_msg(msg):
    """
    Send a message to autowaaave via UDP.

    Messages are forwarded by video_bridge.py to the Teensy via serial,
    where they update the OLED display state.

    Args:
        msg: String message to send
    """
    udp_send.sendto(msg.encode(), (PI1_IP, SEND_PORT))
    print("Sent: {}".format(msg))


def send_clip_list():
    """
    Send the full clip list to automidi.

    Format: LIST:NAME1,NAME2,NAME3,...
    Called on startup and every 10 seconds to keep controller in sync.
    """
    clips = get_clips()
    names = [clean_name(c) for c in clips]
    send_msg('LIST:' + ','.join(names))

# =============================================================================
# IPC COMMAND WRITERS
# =============================================================================

def write_cmd(cmd):
    """
    Write a command for video_player.py to execute.

    video_player.py polls this file and executes commands.

    Args:
        cmd: Dictionary with 'action' key and optional parameters
    """
    with open(CMD_FILE, 'w') as f:
        json.dump(cmd, f)


def write_mixer_cmd(cmd):
    """
    Write a command for mixer.py to execute.

    mixer.py polls this file and updates compositor settings.

    Args:
        cmd: Dictionary with 'action' key and optional parameters
    """
    with open(MIXER_CMD_FILE, 'w') as f:
        json.dump(cmd, f)

# =============================================================================
# COMMAND HANDLER
# =============================================================================

def handle_command(cmd):
    """
    Parse and dispatch incoming commands from automidi.

    Commands arrive as simple strings via UDP. This function parses them
    and writes the appropriate JSON command file for the target process.

    Args:
        cmd: Raw command string (e.g., "NEXT", "PLAY:3", "MIX:64")
    """
    cmd = cmd.strip()
    print("Command: {}".format(cmd))

    # --- Clip navigation commands (-> video_player.py) ---
    if cmd == "NEXT":
        write_cmd({"action": "next"})
    elif cmd == "PREV":
        write_cmd({"action": "prev"})

    # --- Playback control commands (-> video_player.py) ---
    elif cmd == "PLAY":
        write_cmd({"action": "play"})
    elif cmd == "PAUSE":
        write_cmd({"action": "pause"})
    elif cmd == "LOOP_ON":
        write_cmd({"action": "loop_on"})
    elif cmd == "LOOP_OFF":
        write_cmd({"action": "loop_off"})
    elif cmd.startswith("PLAY:"):
        # Jump to specific clip by index
        index = int(cmd.split(':')[1])
        write_cmd({"action": "play_index", "index": index})

    # --- Mixer channel commands (-> mixer.py) ---
    elif cmd.startswith("CH_A:"):
        # Set channel A source (0=clips, 1-3=composite, 4=oscilloscope)
        write_mixer_cmd({"action": "ch_a", "value": int(cmd.split(':')[1])})
    elif cmd.startswith("CH_B:"):
        # Set channel B source
        write_mixer_cmd({"action": "ch_b", "value": int(cmd.split(':')[1])})
    elif cmd.startswith("MIX:"):
        # Set crossfade between A and B (0=full A, 127=full B)
        write_mixer_cmd({"action": "mix", "value": int(cmd.split(':')[1])})

    # --- Luma key commands (-> mixer.py) ---
    elif cmd.startswith("LUMA:"):
        # Set luma key low threshold (pixels below this are keyed out)
        write_mixer_cmd({"action": "luma", "value": int(cmd.split(':')[1])})
    elif cmd.startswith("LUMA_SRC:"):
        # Set luma key source (0=none, 1=clips, 2-4=composite)
        write_mixer_cmd({"action": "luma_src", "value": int(cmd.split(':')[1])})
    elif cmd.startswith("LUMA_HIGH:"):
        # Set luma key high threshold (for band keying)
        write_mixer_cmd({"action": "luma_high", "value": int(cmd.split(':')[1])})
    elif cmd.startswith("LUMA_HIGH_EN:"):
        # Enable/disable high threshold cut
        write_mixer_cmd({"action": "luma_high_en", "value": bool(int(cmd.split(':')[1]))})

# =============================================================================
# STATE WATCHER THREAD
# =============================================================================

def watch_status():
    """
    Monitor video player state and send updates to automidi.

    Runs in background thread. Polls player_state.json every 0.5 seconds
    and sends updates when state changes. This keeps the Teensy's OLED
    displays in sync with actual playback state.

    Sends:
        FILE:name      - When clip changes
        POS:idx:count  - When clip changes (position in list)
        PAUSE:0/1      - When pause state changes
        LOOP:0/1       - When loop state changes
        PROGRESS:p:d   - Every second during playback (position:duration)
    """
    # Track last known state to detect changes
    last_index    = None
    last_paused   = None
    last_loop     = None
    last_time_pos = None
    clips = get_clips()

    while True:
        try:
            if os.path.exists(STATE_FILE):
                # Read current state from video player
                with open(STATE_FILE) as f:
                    state = json.load(f)

                index    = state.get('index', 0)
                paused   = state.get('paused', False)
                loop     = state.get('loop', False)
                time_pos = state.get('time_pos', 0)
                duration = state.get('duration', 0)
                count    = len(clips)

                # Clip changed - send new file info
                if index != last_index:
                    last_index = index
                    last_time_pos = None  # Reset progress tracking
                    if index < len(clips):
                        send_msg("FILE:{}".format(clean_name(clips[index])))
                        send_msg("POS:{}:{}".format(index, count))

                # Pause state changed
                if paused != last_paused:
                    last_paused = paused
                    send_msg("PAUSE:{}".format(1 if paused else 0))

                # Loop state changed
                if loop != last_loop:
                    last_loop = loop
                    send_msg("LOOP:{}".format(1 if loop else 0))

                # Progress update (throttled to ~1/second)
                if duration > 0:
                    # Detect loop restart (time jumps backwards)
                    if last_time_pos is not None and time_pos < last_time_pos - 2:
                        last_time_pos = None
                    # Send update if at least 1 second elapsed
                    if last_time_pos is None or abs(time_pos - last_time_pos) >= 1:
                        last_time_pos = time_pos
                        send_msg("PROGRESS:{}:{}".format(int(time_pos), int(duration)))

        except Exception as e:
            print("Watch error: {}".format(e))

        time.sleep(0.5)  # Poll interval

# =============================================================================
# COMMAND LISTENER THREAD
# =============================================================================

def listen_commands():
    """
    Listen for incoming UDP commands from automidi.

    Runs in background thread. Blocks on UDP receive and dispatches
    commands as they arrive. Commands originate from the Teensy,
    are forwarded by video_bridge.py on autowaaave.
    """
    while True:
        try:
            data, addr = udp_recv.recvfrom(1024)
            handle_command(data.decode())
        except Exception as e:
            print("Listen error: {}".format(e))

# =============================================================================
# MAIN LOOP
# =============================================================================

# Start background threads
threading.Thread(target=watch_status, daemon=True).start()
threading.Thread(target=listen_commands, daemon=True).start()

# Main thread: periodic full state resync
# Every 10 seconds, resend clip list and current state to keep
# automidi in sync (handles reconnection, missed packets, etc.)
while True:
    try:
        send_clip_list()

        # Also resend current state
        if os.path.exists(STATE_FILE):
            with open(STATE_FILE) as f:
                state = json.load(f)
            clips = get_clips()
            index = state.get('index', 0)
            if index < len(clips):
                send_msg("FILE:{}".format(clean_name(clips[index])))
            send_msg("PAUSE:{}".format(1 if state.get('paused', False) else 0))
            send_msg("LOOP:{}".format(1 if state.get('loop', False) else 0))
    except Exception as e:
        print("Resend error: {}".format(e))

    time.sleep(10)  # Resync interval
