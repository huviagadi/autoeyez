"""
video_bridge.py — Serial/UDP Bridge for autowaaave
Part of the autoeyez video synthesis system

Bridges communication between automidi (Teensy) and autoclip (Pi 5):
  - Serial from Teensy (/dev/ttyACM0) → forwards to autoclip UDP:5006
  - UDP from autoclip (port 5005) → forwards to Teensy serial
  - Detects AUTO_WAAAVE_4_5 process running and sends "START" to Teensy
  - Handles MIDI CC7 volume via amixer
  - Listens on UDP:5007 for REBOOT command (TeensyFlaSSH workflow)
"""

import socket
import time
import threading
import serial
import subprocess

SERIAL_PORT  = '/dev/ttyACM0'
BAUD         = 115200
PI2_IP       = '10.0.0.2'
LISTEN_PORT  = 5005
PI2_CMD_PORT = 5006

udp_recv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp_recv.bind(('0.0.0.0', LISTEN_PORT))

udp_send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

def send_to_pi2(cmd):
    udp_send.sendto(cmd.encode(), (PI2_IP, PI2_CMD_PORT))
    print("To Pi2: {}".format(cmd))

def udp_to_serial():
    while True:
        try:
            data, addr = udp_recv.recvfrom(4096)
            msg = data.decode().strip()
            print("Pi2->Teensy: {}".format(msg))
            ser.write((msg + '\n').encode())
        except Exception as e:
            print("UDP recv error: {}".format(e))

def get_serial():
    while True:
        try:
            s = serial.Serial(SERIAL_PORT, BAUD, timeout=0.1)
            print("Serial connected")
            return s
        except Exception as e:
            print("Serial connect failed: {}".format(e))
            time.sleep(2)

def serial_to_pi2():
    global ser
    buf = ''
    while True:
        try:
            if ser.in_waiting:
                char = ser.read().decode('utf-8', errors='ignore')
                if char == '\n':
                    if buf.strip():
                        print("Teensy->Pi2: {}".format(buf))
                        send_to_pi2(buf.strip())
                    buf = ''
                else:
                    buf += char
        except Exception as e:
            print("Serial error: {}".format(e))
            time.sleep(1)
            ser = get_serial()
            buf = ''
            time.sleep(2)
            try:
                ser.write(b'START\n')
                ser.flush()
                print("Resent START after reconnect")
            except Exception as e2:
                print("Failed to resend START: {}".format(e2))

def reboot_teensy():
    try:
        ser.write(b'R\n')
        ser.flush()
        print("Reboot command sent")
    except Exception as e:
        print("Reboot error: {}".format(e))

def listen_for_reboot():
    # Listen on a separate UDP port for reboot command from flash script
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('0.0.0.0', 5007))
    while True:
        try:
            data, addr = sock.recvfrom(1024)
            if data.decode().strip() == 'REBOOT':
                print("Reboot requested")
                reboot_teensy()
        except Exception as e:
            print("Reboot listen error: {}".format(e))

def handle_midi():
    import mido
    while True:
        try:
            ports = mido.get_input_names()
            print("MIDI ports: {}".format(ports))
            with mido.open_input() as port:
                for msg in port:
                    if msg.type == 'control_change' and msg.control == 7:
                        val = msg.value
                        if val <= 100:
                            vol = val
                        else:
                            vol = 100 + int((val - 100) / 27.0 * 50)
                        vol = min(150, vol)
                        subprocess.run(['amixer', 'set', 'Master', '{}%'.format(vol)])
        except Exception as e:
            print("MIDI error: {}".format(e))
            time.sleep(2)

def wait_and_signal_ready():
    print("Waiting for AUTO_WAAAVE to start...")
    while True:
        try:
            result = subprocess.run(['pgrep', '-f', 'AUTO_WAAAVE_4_5'],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if result.returncode == 0:
                print("AUTO_WAAAVE_4_5 detected, waiting for init...")
                time.sleep(5)
                print("Signaling Teensy START...")
                ser.write(b'START\n')
                ser.flush()
                return
        except Exception as e:
            print("Ready check error: {}".format(e))
        time.sleep(1)

ser = get_serial()

threading.Thread(target=wait_and_signal_ready, daemon=True).start()
threading.Thread(target=listen_for_reboot, daemon=True).start()
threading.Thread(target=udp_to_serial, daemon=True).start()

serial_to_pi2()
