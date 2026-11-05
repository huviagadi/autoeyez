from clip_list_transfer import ClipListSender
from patch_transport import serve_remote
"""
video_control.py — Command/State Bridge for autoclip
Part of the autoeyez video synthesis system

This script acts as the communication hub between the automidi controller
(via autowaaave's video_bridge) and the local video player/mixer processes.

Architecture:
    automidi (Teensy)
        → serial → video_bridge.py (on autowaaave)
        → UDP:5006 → THIS SCRIPT
        → acknowledged local sockets → video_player.py / mixer.py

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
import clip_catalog
from command_ipc import CommandSender, PLAYER_COMMAND_SOCKET, MIXER_COMMAND_SOCKET

# =============================================================================
# CONFIGURATION
# =============================================================================

# Network settings - autowaaave is the hub at 10.0.0.1
PI1_IP    = '10.0.0.1'   # autowaaave IP address
SEND_PORT = 5005         # Port to send state updates TO autowaaave
RECV_PORT = 5006         # Port to receive commands FROM autowaaave

# File paths
CLIPS_DIR = '/home/admin/clips/'   # MP4 clips directory
STATE_FILE = '/tmp/player_state.json' # Video player writes state here
player_commands = None
MIXER_STATE_FILE = '/tmp/mixer_state.json'  # Mixer writes state here (unused currently)
mixer_commands = None
KEYPAD_SOCKET    = '/tmp/keypad_cue.sock'   # side_keypad.py patch cue storage

# =============================================================================
# SOCKET SETUP
# =============================================================================

# UDP socket for sending state updates to autowaaave
udp_send = None

# UDP socket for receiving commands from autowaaave
udp_recv = None


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
    return clip_catalog.names()


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


clip_list_transfer = None
folder_list_message = None

def send_clip_list():
    global folder_list_message
    catalog = clip_catalog.update()
    names = [clean_name(c) for c in catalog['order']]
    folder_list_message = clip_catalog.folder_message(catalog, names)
    clip_list_transfer.submit(names)

# =============================================================================
# IPC COMMAND WRITERS
# =============================================================================

def write_cmd(cmd):
    """Queue an ordered player action without blocking UDP reception."""
    player_commands.send(cmd)


def write_mixer_cmd(cmd):
    """Queue controls; pending continuous values may merge, actions stay ordered."""
    mixer_commands.send(cmd)


def send_keypad_cmd(cmd):
    """Send a video-patch cue save/load event to the side keypad service."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        sock.sendto(json.dumps(cmd).encode(), KEYPAD_SOCKET)
    except OSError as error:
        print("Keypad command failed: {}".format(error))
    finally:
        sock.close()

# =============================================================================
# COMMAND HANDLER
# =============================================================================

def handle_command(cmd):
    """
    Parse and dispatch incoming commands from automidi.

    Commands arrive as simple strings via UDP. This function parses them
    and queues an acknowledged local command for the target frame loop.

    Args:
        cmd: Raw command string (e.g., "NEXT", "PLAY:3", "MIX:64")
    """
    cmd = cmd.strip()
    if cmd.startswith('SHADER_FFT:'):
        try:
            values = [int(value) for value in cmd[11:].split(',')]
            if len(values) == 3 and all(0 <= value <= 1016 for value in values):
                write_mixer_cmd({'action': 'shader_fft',
                                 'value': [value / 127.0 for value in values]})
        except (ValueError, BufferError):
            pass
        return
    print("Command: {}".format(cmd))

    if cmd.startswith('SHADER_BAND:'):
        band = int(cmd.split(':', 1)[1])
        if band in (-1, 0, 1, 2):
            send_keypad_cmd({'action': 'shader_band', 'band': band})
        return
    if cmd.startswith(('SHADER_AUDIO_SAVE:', 'SHADER_AUDIO_LOAD:')):
        name, raw_slot = cmd.split(':', 1)
        slot = int(raw_slot)
        if 0 <= slot < 32:
            send_keypad_cmd({'action': name.lower(), 'slot': slot})
        return

    if cmd == "SOURCE_STATE?":
        with open(MIXER_STATE_FILE) as f:state=json.load(f)
        values=[state['ch_a'],state['ch_b'],state['mix'],state['luma'],state['luma_high'],int(state['luma_high_enabled']),state['luma_src'],int(state['luma']>0)]
        send_msg('SOURCE_STATE:'+','.join(map(str,values)))
        return

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
    elif cmd.startswith("PLAYLOOP:"):
        # Jump to clip and set loop state atomically (from patch load)
        parts = cmd.split(':')
        index = int(parts[1])
        loop = int(parts[2]) == 1
        write_cmd({"action": "play_loop", "index": index, "loop": loop})
    elif cmd.startswith("PLAY:"):
        # Jump to specific clip by index
        index = int(cmd.split(':')[1])
        write_cmd({"action": "play_index", "index": index})
    elif cmd.startswith("CUES_SAVE:"):
        slot = max(0, min(31, int(cmd.split(':')[1])))
        send_keypad_cmd({"action": "save_patch", "slot": slot})
    elif cmd.startswith("CUES_LOAD:"):
        slot = max(0, min(31, int(cmd.split(':')[1])))
        send_keypad_cmd({"action": "load_patch", "slot": slot})

    # --- Mixer channel commands (-> mixer.py) ---
    elif cmd.startswith("CH_A:"):
        # Set channel A source (0=clips, 1-3=composite, 4=EXT USB, 5+=math)
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
    elif cmd.startswith("LUMA_FEATHER:"):
        write_mixer_cmd({"action":"luma_feather","value":max(0,min(4,int(cmd.split(':')[1])))})
    elif cmd.startswith("LUMA_SRC:"):
        # Set luma key source (0=none, then channel-source index + 1)
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
        EXT_USB:0/1    - When EXT USB availability changes
    """
    # Track last known state to detect changes
    last_index    = None
    last_paused   = None
    last_loop     = None
    last_time_pos = None
    last_ext_usb  = None
    clips = get_clips()
    last_catalog_read = time.monotonic()
    while True:
        try:
            if time.monotonic() - last_catalog_read >= 5:
                clips = get_clips()
                last_catalog_read = time.monotonic()
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

            # Check mixer state for EXT USB availability
            if os.path.exists(MIXER_STATE_FILE):
                with open(MIXER_STATE_FILE) as f:
                    mixer_state = json.load(f)
                ext_usb = mixer_state.get('ext_usb_available', False)
                if ext_usb != last_ext_usb:
                    last_ext_usb = ext_usb
                    send_msg("EXT_USB:{}".format(1 if ext_usb else 0))

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
            message=data.decode()
            if clip_list_transfer.acknowledge(message):
                if folder_list_message: send_msg(folder_list_message)
            else: handle_command(message)
        except Exception as e:
            print("Listen error: {}".format(e))

# =============================================================================
# MAIN LOOP
# =============================================================================

def main():
    global player_commands, mixer_commands, udp_send, udp_recv, clip_list_transfer
    player_commands = CommandSender(PLAYER_COMMAND_SOCKET, 'player')
    mixer_commands = CommandSender(MIXER_COMMAND_SOCKET, 'mixer')
    udp_send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_recv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_recv.bind(('0.0.0.0', RECV_PORT))  # Listen on all interfaces
    clip_list_transfer = ClipListSender(send_msg)

    # Start background threads
    threading.Thread(target=clip_list_transfer.run,daemon=True,name='clip-list-retry').start()
    threading.Thread(target=serve_remote,daemon=True,name='patch-rpc').start()
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


if __name__ == "__main__":
    main()
