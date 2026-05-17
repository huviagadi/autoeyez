"""
video_control.py — Command/State Bridge for autoclip
Part of the autoeyez video synthesis system

Bridges network commands to IPC files:
  - Receives UDP commands on port 5006 from automidi (via autowaaave bridge)
  - Writes commands to /tmp/player_cmd.json and /tmp/mixer_cmd.json
  - Reads state from player/mixer and sends updates to autowaaave on port 5005

Commands: NEXT, PREV, PLAY, PAUSE, LOOP_ON/OFF, PLAY:n, CH_A:n, etc.
"""

import socket
import os
import time
import threading
import json

PI1_IP    = '10.0.0.1'
SEND_PORT = 5005
RECV_PORT = 5006
CLIPS_DIR = '/boot/firmware/clips/'
STATE_FILE = '/tmp/player_state.json'
CMD_FILE   = '/tmp/player_cmd.json'
MIXER_STATE_FILE = '/tmp/mixer_state.json'
MIXER_CMD_FILE   = '/tmp/mixer_cmd.json'

udp_send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp_recv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp_recv.bind(('0.0.0.0', RECV_PORT))

def clean_name(filename):
    name = os.path.splitext(filename)[0]
    if '_' in name:
        name = name.split('_', 1)[1]
    return name.upper()

def get_clips():
    return sorted([f for f in os.listdir(CLIPS_DIR) if f.endswith('.mp4')])

def send_msg(msg):
    udp_send.sendto(msg.encode(), (PI1_IP, SEND_PORT))
    print("Sent: {}".format(msg))

def send_clip_list():
    clips = get_clips()
    names = [clean_name(c) for c in clips]
    send_msg('LIST:' + ','.join(names))

def write_cmd(cmd):
    with open(CMD_FILE, 'w') as f:
        json.dump(cmd, f)

def write_mixer_cmd(cmd):
    with open(MIXER_CMD_FILE, 'w') as f:
        json.dump(cmd, f)

def handle_command(cmd):
    cmd = cmd.strip()
    print("Command: {}".format(cmd))
    if cmd == "NEXT":
        write_cmd({"action": "next"})
    elif cmd == "PREV":
        write_cmd({"action": "prev"})
    elif cmd == "PLAY":
        write_cmd({"action": "play"})
    elif cmd == "PAUSE":
        write_cmd({"action": "pause"})
    elif cmd == "LOOP_ON":
        write_cmd({"action": "loop_on"})
    elif cmd == "LOOP_OFF":
        write_cmd({"action": "loop_off"})
    elif cmd.startswith("PLAY:"):
        index = int(cmd.split(':')[1])
        write_cmd({"action": "play_index", "index": index})
    elif cmd.startswith("CH_A:"):
        write_mixer_cmd({"action": "ch_a", "value": int(cmd.split(':')[1])})
    elif cmd.startswith("CH_B:"):
        write_mixer_cmd({"action": "ch_b", "value": int(cmd.split(':')[1])})
    elif cmd.startswith("MIX:"):
        write_mixer_cmd({"action": "mix", "value": int(cmd.split(':')[1])})
    elif cmd.startswith("LUMA:"):
        write_mixer_cmd({"action": "luma", "value": int(cmd.split(':')[1])})
    elif cmd.startswith("LUMA_SRC:"):
        write_mixer_cmd({"action": "luma_src", "value": int(cmd.split(':')[1])})
    elif cmd.startswith("LUMA_HIGH:"):
        write_mixer_cmd({"action": "luma_high", "value": int(cmd.split(':')[1])})
    elif cmd.startswith("LUMA_HIGH_EN:"):
        write_mixer_cmd({"action": "luma_high_en", "value": bool(int(cmd.split(':')[1]))})

def watch_status():
    last_index    = None
    last_paused   = None
    last_loop     = None
    last_time_pos = None
    clips = get_clips()

    while True:
        try:
            if os.path.exists(STATE_FILE):
                with open(STATE_FILE) as f:
                    state = json.load(f)
                index    = state.get('index', 0)
                paused   = state.get('paused', False)
                loop     = state.get('loop', False)
                time_pos = state.get('time_pos', 0)
                duration = state.get('duration', 0)
                count    = len(clips)

                if index != last_index:
                    last_index = index
                    last_time_pos = None
                    if index < len(clips):
                        send_msg("FILE:{}".format(clean_name(clips[index])))
                        send_msg("POS:{}:{}".format(index, count))

                if paused != last_paused:
                    last_paused = paused
                    send_msg("PAUSE:{}".format(1 if paused else 0))

                if loop != last_loop:
                    last_loop = loop
                    send_msg("LOOP:{}".format(1 if loop else 0))

                if duration > 0:
                    # Reset if time jumped backwards (loop)
                    if last_time_pos is not None and time_pos < last_time_pos - 2:
                        last_time_pos = None
                    if last_time_pos is None or abs(time_pos - last_time_pos) >= 1:
                        last_time_pos = time_pos
                        send_msg("PROGRESS:{}:{}".format(int(time_pos), int(duration)))

        except Exception as e:
            print("Watch error: {}".format(e))
        time.sleep(0.5)

def listen_commands():
    while True:
        try:
            data, addr = udp_recv.recvfrom(1024)
            handle_command(data.decode())
        except Exception as e:
            print("Listen error: {}".format(e))

threading.Thread(target=watch_status, daemon=True).start()
threading.Thread(target=listen_commands, daemon=True).start()

while True:
    try:
        send_clip_list()
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
    time.sleep(10)
