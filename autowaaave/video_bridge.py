"""
video_bridge.py — Serial/UDP Bridge for autowaaave
Part of the autoeyez video synthesis system

This script runs on the autowaaave Pi 3B+ and acts as the central
communication hub for the autoeyez system. It bridges messages between
the automidi Teensy controller and the autoclip video processor.

Architecture:
    automidi (Teensy 4.1)
        ↔ USB Serial (/dev/ttyACM0)
        ↔ THIS SCRIPT (on autowaaave)
        ↔ UDP network
        ↔ autoclip (Pi 5)

Message flow:
    Teensy → Serial → video_bridge → UDP:5006 → video_control.py (autoclip)
    video_control.py → UDP:5005 → video_bridge → Serial → Teensy

Additional features:
    - Process detection: Waits for AUTO_WAAAVE_4_5 to start, then signals Teensy
    - MIDI volume: Handles MIDI CC7 (volume) messages to control system volume
    - Remote reboot: Listens on UDP:5007 for REBOOT command (TeensyFlaSSH workflow)

Network ports:
    5005 - Listen for state updates from autoclip (→ forward to Teensy)
    5006 - autoclip listens here (← we send commands here)
    5007 - Listen for reboot commands from flash script

Systemd:
    Runs via video_bridge.service
"""

import socket
import time
import threading
import serial
import subprocess

# =============================================================================
# CONFIGURATION
# =============================================================================

# Serial connection to Teensy (USB CDC ACM)
SERIAL_PORT  = '/dev/ttyACM0'
BAUD         = 115200

# Network settings
PI2_IP       = '10.0.0.2'    # autoclip IP address
LISTEN_PORT  = 5005          # Port we listen on for state updates from autoclip
PI2_CMD_PORT = 5006          # Port autoclip listens on for commands

# =============================================================================
# SOCKET SETUP
# =============================================================================

# UDP socket for receiving state updates from autoclip
udp_recv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
udp_recv.bind(('0.0.0.0', LISTEN_PORT))  # Listen on all interfaces

# UDP socket for sending commands to autoclip
udp_send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# =============================================================================
# NETWORK FUNCTIONS
# =============================================================================

def send_to_pi2(cmd):
    """
    Send a command string to autoclip via UDP.

    Commands are forwarded to video_control.py on the autoclip Pi,
    which parses them and writes to the appropriate JSON command file.

    Args:
        cmd: Command string (e.g., "NEXT", "PLAY:3", "MIX:64")
    """
    udp_send.sendto(cmd.encode(), (PI2_IP, PI2_CMD_PORT))
    print("To Pi2: {}".format(cmd))


def udp_to_serial():
    """
    Forward UDP messages from autoclip to Teensy serial.

    Runs in background thread. Receives state updates from autoclip
    (clip names, playback position, etc.) and forwards them to the
    Teensy for OLED display updates.

    Messages received: LIST:..., FILE:..., POS:..., PAUSE:..., etc.
    """
    while True:
        try:
            data, addr = udp_recv.recvfrom(4096)
            msg = data.decode().strip()
            print("Pi2->Teensy: {}".format(msg))
            # Forward to Teensy with newline terminator
            ser.write((msg + '\n').encode())
        except Exception as e:
            print("UDP recv error: {}".format(e))

# =============================================================================
# SERIAL FUNCTIONS
# =============================================================================

def get_serial():
    """
    Connect to Teensy serial port with retry.

    The Teensy appears as /dev/ttyACM0 when connected via USB.
    This function retries until connection succeeds, handling
    cases where the Teensy isn't connected or is being flashed.

    Returns:
        serial.Serial object connected to Teensy
    """
    while True:
        try:
            s = serial.Serial(SERIAL_PORT, BAUD, timeout=0.1)
            print("Serial connected")
            return s
        except Exception as e:
            print("Serial connect failed: {}".format(e))
            time.sleep(2)  # Wait before retry


def serial_to_pi2():
    """
    Forward serial messages from Teensy to autoclip UDP.

    Runs as main thread. Reads characters from serial, buffers
    until newline, then forwards complete lines to autoclip.
    Handles reconnection if serial connection drops.

    Commands forwarded: NEXT, PREV, PLAY, PAUSE, CH_A:n, MIX:n, etc.
    """
    global ser
    buf = ''  # Character buffer for building lines

    while True:
        try:
            if ser.in_waiting:
                # Read one character at a time for line buffering
                char = ser.read().decode('utf-8', errors='ignore')

                if char == '\n':
                    # Complete line received - forward it
                    if buf.strip():
                        print("Teensy->Pi2: {}".format(buf))
                        send_to_pi2(buf.strip())
                    buf = ''
                else:
                    buf += char

        except Exception as e:
            # Serial error - likely Teensy disconnected
            print("Serial error: {}".format(e))
            time.sleep(1)

            # Reconnect to serial
            ser = get_serial()
            buf = ''  # Clear partial buffer

            # Wait for Teensy to initialize
            time.sleep(2)

            # Resend START signal after reconnection
            try:
                ser.write(b'START\n')
                ser.flush()
                print("Resent START after reconnect")
            except Exception as e2:
                print("Failed to resend START: {}".format(e2))

# =============================================================================
# TEENSY REBOOT SUPPORT
# =============================================================================

def reboot_teensy():
    """
    Send reboot command to Teensy.

    Used by TeensyFlaSSH workflow to trigger a soft reboot
    before flashing new firmware.
    """
    try:
        ser.write(b'R\n')
        ser.flush()
        print("Reboot command sent")
    except Exception as e:
        print("Reboot error: {}".format(e))


def listen_for_reboot():
    """
    Listen for remote reboot commands via UDP.

    Runs in background thread. The TeensyFlaSSH workflow sends
    a REBOOT command to port 5007 before attempting to flash
    new firmware. This triggers reboot_teensy().
    """
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

# =============================================================================
# MIDI VOLUME CONTROL
# =============================================================================

def handle_midi():
    """
    Handle MIDI CC7 (volume) messages.

    Runs in background thread. Opens the default MIDI input and
    listens for Control Change 7 (volume) messages. Maps MIDI
    value (0-127) to system volume via amixer.

    Volume mapping:
        0-100   → 0-100% (linear)
        100-127 → 100-150% (compressed, for boost)
    """
    import mido

    while True:
        try:
            ports = mido.get_input_names()
            print("MIDI ports: {}".format(ports))

            with mido.open_input() as port:
                for msg in port:
                    # Only handle Control Change 7 (volume)
                    if msg.type == 'control_change' and msg.control == 7:
                        val = msg.value

                        # Map MIDI value to volume percentage
                        if val <= 100:
                            vol = val  # Linear 0-100
                        else:
                            # Compress 101-127 into 100-150 range (boost)
                            vol = 100 + int((val - 100) / 27.0 * 50)

                        vol = min(150, vol)  # Cap at 150%

                        # Set system volume via amixer
                        subprocess.run(['amixer', 'set', 'Master', '{}%'.format(vol)])

        except Exception as e:
            print("MIDI error: {}".format(e))
            time.sleep(2)  # Wait before retry

# =============================================================================
# PROCESS DETECTION
# =============================================================================

def wait_and_signal_ready():
    """
    Wait for AUTO_WAAAVE_4_5 to start, then signal Teensy.

    Runs in background thread. Polls for the openFrameworks app
    process. Once detected, waits 5 seconds for initialization,
    then sends START command to Teensy.

    The START command tells the Teensy that the video system is
    ready, enabling its control outputs.
    """
    print("Waiting for AUTO_WAAAVE to start...")

    while True:
        try:
            # Check if AUTO_WAAAVE_4_5 process is running
            result = subprocess.run(['pgrep', '-f', 'AUTO_WAAAVE_4_5'],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)

            if result.returncode == 0:
                print("AUTO_WAAAVE_4_5 detected, waiting for init...")
                time.sleep(5)  # Wait for app to initialize

                print("Signaling Teensy START...")
                ser.write(b'START\n')
                ser.flush()
                return  # Done - exit thread

        except Exception as e:
            print("Ready check error: {}".format(e))

        time.sleep(1)  # Poll every second

# =============================================================================
# MAIN
# =============================================================================

# Connect to Teensy serial port
ser = get_serial()

# Start background threads
threading.Thread(target=wait_and_signal_ready, daemon=True).start()
threading.Thread(target=listen_for_reboot, daemon=True).start()
threading.Thread(target=udp_to_serial, daemon=True).start()

# Run serial→UDP forwarding in main thread (blocks forever)
serial_to_pi2()
