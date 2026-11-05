"""
video_bridge.py — Serial/UDP Bridge for autowaaave
Part of the autoeyez video synthesis system

This script runs on the autowaaave Pi 5 and acts as the central
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
    - Process detection: Waits for the active AUTO_WAAAVE build, then signals
      the Teensy.
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
import subprocess
import os
import uuid
from serial_link import SerialLink
from patch_coordinator import PatchCoordinator
patch_transactions=None
from display_worker import DisplayWorker
from envelope_sync import EnvelopeSync
from band_sync import BandSync
from app_health import read_status, LegacyFrames, publisher_claims_pid
from hdmi_audio import handle_audio_pass

# =============================================================================
# CONFIGURATION
# =============================================================================

# Serial connection to Teensy (USB CDC ACM)
BAUD         = 115200

# Network settings
PI2_IP       = '10.0.0.2'    # autoclip IP address
LISTEN_PORT  = 5005          # Port we listen on for state updates from autoclip
PI2_CMD_PORT = 5006          # Port autoclip listens on for commands

# FFT file paths (shared with AUTO_WAAAVE)
FFT_FILE     = '/dev/shm/fft_values'
FFT_SMOOTH   = '/dev/shm/fft_smooth'
FFT_ATTACK   = '/dev/shm/fft_attack'
FFT_DECAY    = '/dev/shm/fft_decay'
FFT_BDECAY   = '/dev/shm/fft_bdecay'
FFT_HIT      = '/dev/shm/fft_hit'
FFT_SLIDE    = '/dev/shm/fft_slide'
FFT_ENV_UPDATE = '/dev/shm/fft_env_update'  # Written by autowaaave on audio patch load
VIDEO_OUTPUT_SCRIPT = '/home/admin/set_video_output.sh'
LUMA_TRACK_FILE = '/dev/shm/luma_track'
SHOW_OVERLAY_FILE = '/dev/shm/show_overlay'
CONTROLLER_GENERATION = '/dev/shm/autoeyez_controller_generation'
serial_generation_published = None
bridge_identity = uuid.uuid4().hex
start_ack_generation = None
start_ack_deadline = 0
envelope_sync = None
band_sync = None

# =============================================================================
# SOCKET SETUP
# =============================================================================

udp_recv = None
udp_send = None

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
            if not (msg.startswith('BEAT:') or msg == 'BEAT_STOP'):
                print("Pi2->Teensy: {}".format(msg))
            # Forward to Teensy with newline terminator
            ser.send((msg + '\n').encode())
        except Exception as e:
            print("UDP recv error: {}".format(e))

# =============================================================================
# SERIAL FUNCTIONS
# =============================================================================

def wait_patch_profiles(text):
    fields=dict(line.split('=',1) for line in text.splitlines() if '=' in line)
    desired=[int(fields.get('env_'+name,value)) for name,value in zip(('smooth','attack','decay','bdecay','hit','slide'),(38,64,102,102,96,64))]
    shaped=int(fields.get('env_mode','0'));bands=fields.get('teensy_bands_profile','LEGACY')
    deadline=time.monotonic()+4
    while time.monotonic()<deadline:
        try:
            with open('/dev/shm/autoeyez_fft_envelope') as f:snapshot=f.read().split()
            with open('/dev/shm/fft_env_status') as f:env_status=f.read().split()
            with open('/dev/shm/fft_band_active') as f:band_status=f.read().split()
            if ser.connected and envelope_sync is not None and band_sync is not None and len(snapshot)==9 and envelope_sync.sent==(snapshot[1],ser.generation) and int(snapshot[2])==shaped and list(map(int,snapshot[3:]))==desired and env_status==[snapshot[1],'controller-accepted'] and len(band_status)==6 and band_status[0]==band_sync.identity and int(band_status[1])==ser.generation and band_status[2]==bands and band_status[3]==bands and band_status[4]=='CONFIRMED':return
        except (OSError,ValueError):pass
        time.sleep(.02)
    raise TimeoutError('controller did not confirm audio patch profiles')

def handle_serial_line(line):
    global start_ack_generation, start_ack_deadline
    if line.startswith("PTX_RECOVER:") or line.startswith("PTX_UNDO:"):
        if patch_transactions is not None:patch_transactions.recover(line.split(':',1)[1],undo=line.startswith("PTX_UNDO:"))
        return
    if line.startswith("PTX:"):
        if patch_transactions is not None:patch_transactions.receive(line)
        return
    if line == 'GOT_START' or line.startswith('Boot finalized'):
        start_ack_generation = ser.generation
        start_ack_deadline = time.monotonic() + 2
        print('Teensy acknowledged START' if line == 'GOT_START' else 'Teensy boot completion observed', flush=True)
        return
    if line.startswith('BANDS_OK:'):
        if band_sync is not None: band_sync.acknowledge(line)
        return
    if line == 'BANDS_ERROR':
        print('Controller rejected band profile', flush=True)
        return
    if line == 'ENV_OK':
        if envelope_sync is not None: envelope_sync.acknowledge()
        return
    # Intercept FFT commands (don't forward to Pi2)
    if line.startswith("FFT:"):
        handle_fft(line)
        if len(line[4:].split(',')) == 3:
            mark_controller_ready()
    elif line.startswith("FFT_SMOOTH:"):
        handle_fft_param(line, FFT_SMOOTH)
    elif line.startswith("FFT_ATTACK:"):
        handle_fft_param(line, FFT_ATTACK)
    elif line.startswith("FFT_DECAY:"):
        handle_fft_param(line, FFT_DECAY)
    elif line.startswith("FFT_BDECAY:"):
        handle_fft_param(line, FFT_BDECAY)
    elif line.startswith("FFT_HIT:"):
        handle_fft_param(line, FFT_HIT)
    elif line.startswith("FFT_SLIDE:"):
        handle_fft_param(line, FFT_SLIDE)
    elif line.startswith("VIDEO_OUT:"):
        handle_video_output(line)
    elif line.startswith("AUDIO_PASS:"):
        handle_audio_pass(line)
    elif line.startswith("LUMA_TRACK:"):
        handle_luma_track(line)
    elif (line.startswith("HOUSECARD:") or
          line.startswith("HOUSELOGO:") or
          line.startswith("ANNLOGO:")):
        handle_show_overlay(line)
    else:
        # Forward other commands to autoclip
        print("Teensy->Pi2: {}".format(line))
        send_to_pi2(line)

# =============================================================================
# VIDEO OUTPUT CONTROL
# =============================================================================

def handle_video_output(cmd):
    display_worker.submit(cmd.split(':', 1)[1].strip().upper())


def publish_controller_generation(ready):
    temporary = CONTROLLER_GENERATION + '.tmp'
    with open(temporary, 'w') as f:
        f.write('{}:{} {}\n'.format(bridge_identity, ser.generation, int(ready)))
    os.replace(temporary, CONTROLLER_GENERATION)


def mark_controller_ready():
    global serial_generation_published
    if serial_generation_published != ser.generation:
        publish_controller_generation(True)
        serial_generation_published = ser.generation


def controller_ready_tick():
    # Gain=0 can suppress FFT packets. A START acknowledgment plus the current
    # firmware's initialization grace still permits MIDI re-subscription.
    if start_ack_generation == ser.generation and time.monotonic() >= start_ack_deadline:
        mark_controller_ready()


def serial_connected():
    publish_controller_generation(False)
    sync_luma_track()


def sync_luma_track():
    """Default ON each boot; preserve an explicit OFF across bridge reconnects."""
    try:
        with open(LUMA_TRACK_FILE, 'x') as state_file:
            state_file.write('1')
    except FileExistsError:
        pass
    with open(LUMA_TRACK_FILE) as state_file:
        enabled = state_file.read().strip() != '0'
    ser.send(('LUMA_TRACK:{}\n'.format('ON' if enabled else 'OFF')).encode())


def handle_luma_track(cmd):
    """Set global adaptive luma tracking and confirm the applied state."""
    global ser
    enabled = cmd.split(':', 1)[1].strip().upper() in ('1', 'ON', 'TRUE')
    try:
        tmp_path = LUMA_TRACK_FILE + '.tmp'
        with open(tmp_path, 'w') as state_file:
            state_file.write('1' if enabled else '0')
        os.rename(tmp_path, LUMA_TRACK_FILE)
        reply = 'LUMA_TRACK:{}'.format('ON' if enabled else 'OFF')
        ser.send((reply + '\n').encode())
        print(reply)
    except Exception as e:
        print("Luma track update failed: {}".format(e))


def handle_show_overlay(cmd):
    """Select one mutually exclusive final-output show card/logo overlay."""
    name, raw_value = cmd.split(':', 1)
    modes = {'HOUSECARD': 1, 'HOUSELOGO': 2, 'ANNLOGO': 3}
    requested_mode = modes[name]
    enabled = raw_value.strip() in ('1', 'ON', 'TRUE')
    current_mode = 0
    try:
        with open(SHOW_OVERLAY_FILE) as state_file:
            current_mode = int(state_file.read().strip() or '0')
    except (IOError, OSError, ValueError):
        pass
    new_mode = requested_mode if enabled else (
        0 if current_mode == requested_mode else current_mode)
    try:
        tmp_path = SHOW_OVERLAY_FILE + '.tmp'
        with open(tmp_path, 'w') as state_file:
            state_file.write(str(new_mode))
        os.rename(tmp_path, SHOW_OVERLAY_FILE)
        print("SHOW_OVERLAY:{}".format(new_mode))
    except Exception as e:
        print("Show overlay update failed: {}".format(e))

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
        ser.send(b'R\n', confirmed=True)
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
# FFT DATA HANDLING
# =============================================================================

def handle_fft(cmd):
    """
    Write FFT band values to file for AUTO_WAAAVE to read.
    Format: "FFT:low,mid,high" → writes "low mid high" to file
    Uses atomic write (write to temp, then rename) to prevent race conditions.
    """
    try:
        parts = cmd[4:].split(",")
        if len(parts) == 3:
            tmp_path = FFT_FILE + ".tmp"
            with open(tmp_path, "w") as f:
                f.write("{} {} {}".format(parts[0], parts[1], parts[2]))
            os.rename(tmp_path, FFT_FILE)
    except Exception as e:
        print("FFT write error: {}".format(e))


def handle_fft_param(cmd, filepath):
    try:
        name = cmd.split(':', 1)[0][4:].lower()
        value = int(cmd.split(':', 1)[1])
        if not 0 <= value <= 127: raise ValueError('out-of-range envelope value')
        envelope_sync.set_control(name, value)
    except (ValueError, OSError) as error:
        print('Envelope control rejected: {}'.format(error), flush=True)


# =============================================================================
# ENVELOPE SYNC (Pi → Teensy)
# =============================================================================

def monitor_env_update():
    while True:
        try:
            envelope_sync.tick()
            band_sync.tick()
        except Exception as error: print('Envelope sync error: {}'.format(error), flush=True)
        time.sleep(0.05)


# =============================================================================
# PROCESS DETECTION
# =============================================================================

APP_PROCESS_NAMES = ('pi_5_stable_04', 'pi_5_stable_03', 'AUTO_WAAAVE_4_5')


def wait_and_signal_ready():
    """START requires actual frame evidence; repeat after app/USB generation changes."""
    legacy = LegacyFrames()
    signaled = None
    observed_pid = None
    while True:
        try:
            result = subprocess.run(['pgrep', '-x', 'pi_5_stable_04'],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=1)
            pids = [int(p) for p in result.stdout.split()]
            pid = pids[0] if len(pids) == 1 else None
            if pid != observed_pid:
                legacy = LegacyFrames() # Do not carry frame evidence across app restarts.
                observed_pid = pid
            status = read_status(pid) if pid else None
            ready = bool(status and status.get('video_ready') and status.get('input_frames', 0) > 0)
            if status is None and pid and not publisher_claims_pid(pid):
                ready = legacy.ready() # Observe new live-frame log lines, not process presence alone.
            key = (pid, ser.generation)
            if ready and ser.connected and key != signaled:
                ser.send(b'START\n', confirmed=True)
                sync_luma_track()
                signaled = key
                print('START sent for app={} serial_generation={}'.format(*key), flush=True)
        except Exception as error:
            print('Ready check: {}'.format(error), flush=True)
        time.sleep(.5)

# =============================================================================
# MAIN
# =============================================================================

def main():
    global ser, display_worker, udp_recv, udp_send, envelope_sync, band_sync, patch_transactions
    udp_recv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_recv.bind(('0.0.0.0', LISTEN_PORT))
    udp_send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ser = SerialLink(handle_serial_line, on_connect=serial_connected, autostart=False, on_tick=controller_ready_tick)
    patch_transactions = PatchCoordinator(lambda text: ser.send((text+'\n').encode()), profiles=wait_patch_profiles)
    display_worker = DisplayWorker(lambda data: ser.send(data))
    ser.start()
    threading.Thread(target=wait_and_signal_ready, daemon=True).start()
    threading.Thread(target=listen_for_reboot, daemon=True).start()
    envelope_sync = EnvelopeSync(ser)
    band_sync = BandSync(ser)
    threading.Thread(target=udp_to_serial, daemon=True).start()
    threading.Thread(target=monitor_env_update, daemon=True).start()
    while True:
        time.sleep(1)
        if not ser.thread.is_alive() or not display_worker.thread.is_alive():
            raise RuntimeError('Bridge worker exited; service must restart')

if __name__ == '__main__':
    main()
