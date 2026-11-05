"""One serial owner; bounded line writes and state replay on stable-USB reconnect."""
from collections import deque
from dataclasses import dataclass,field
import glob
import os
import threading
import time

STATE_KEYS={'LIST','FILE','POS','PAUSE','LOOP','PROGRESS','LUMA_TRACK','VIDEO_OUT','EXT_USB'}

def teensy_path():
    override=os.environ.get('AUTOEYEZ_TEENSY_SERIAL')
    if override:
        if not os.path.exists(override):raise FileNotFoundError(override)
        return override
    matches=[p for p in glob.glob('/dev/serial/by-id/usb-Teensyduino_Teensy_*_18383700-if00') if os.path.exists(p)]
    if len(matches)!=1:raise OSError('Expected one stable Teensy 18383700 identity; found {}'.format(len(matches)))
    return matches[0]

@dataclass
class Ticket:
    data:bytes
    key:str|None
    done:threading.Event=field(default_factory=threading.Event)
    success:bool=False
    created:float=field(default_factory=time.monotonic)

class SerialLink:
    def __init__(self,on_line,on_connect=lambda:None,opener=None,resolver=teensy_path,retry=1.0,autostart=True,on_tick=lambda:None):
        if opener is None:
            import serial
            # USB CDC can pause briefly while the host services video and
            # audio. A 50 ms write deadline treated those pauses as a broken
            # controller and caused a full 1-2 second reconnect.
            opener=lambda path:serial.Serial(path,115200,timeout=.005,write_timeout=.2,exclusive=True)
        self.on_line,self.on_connect,self.on_tick=on_line,on_connect,on_tick
        self.opener,self.resolver,self.retry=opener,resolver,retry
        self.lock=threading.Lock();self.pending=deque();self.cache={}
        self.connected=False;self.generation=0;self.stop=threading.Event()
        self.thread=threading.Thread(target=self._run,name='teensy-serial-owner',daemon=True)
        if autostart:self.thread.start()

    def start(self):
        self.thread.start()

    def send(self,data,confirmed=False):
        if not isinstance(data,bytes) or len(data)>8192 or not data.endswith(b'\n') or data.count(b'\n')!=1:
            raise ValueError('Expected one bounded newline-terminated serial line')
        family=data.split(b':',1)[0].decode('ascii',errors='ignore').strip()
        key=family if family in STATE_KEYS else None
        ticket=Ticket(data,key)
        with self.lock:
            if self.stop.is_set():raise ConnectionError('Serial link stopped')
            if key:self.cache[key]=data
            if not self.connected:
                if key:return ticket  # Latest state will replay on reconnect.
                raise ConnectionError('Teensy disconnected; action was not queued')
            if key:
                for old in reversed(self.pending):
                    if old.key is None:break
                    if old.key==key:
                        old.data=data
                        return old
            if len(self.pending)>=128:
                raise BufferError('Serial output queue full')
            if data==b'R\n':self.pending.appendleft(ticket)
            else:self.pending.append(ticket)
        if confirmed:
            if not ticket.done.wait(.5) or not ticket.success:
                raise TimeoutError('Serial line not confirmed written within 500 ms')
        return ticket

    def _disconnect(self):
        with self.lock:
            self.connected=False
            while self.pending:self.pending.popleft().done.set()

    def _run(self):
        while not self.stop.is_set():
            port=None
            recovery_error=None
            try:
                open_started=time.monotonic()
                path=self.resolver();port=self.opener(path)
                open_ms=(time.monotonic()-open_started)*1000
                # End any pre-disconnect partial line. NUL makes a partial R
                # fail the firmware's exact one-character reboot check.
                if port.write(b'\x00\n')!=2:raise OSError('Partial serial resync')
                with self.lock:
                    self.connected=True;self.generation+=1
                    self.pending.extend(Ticket(data,key) for key,data in self.cache.items())
                print('Serial connected: {} generation={} open_ms={:.1f}'.format(path,self.generation,open_ms),flush=True)
                try:self.on_connect()
                except Exception as error:print('Serial connection callback: {}'.format(error),flush=True)
                buf=bytearray();discard=False
                while not self.stop.is_set():
                    # Drain incoming FFT packets between small output bursts.
                    # The periodic clip-list replay can otherwise enqueue 36
                    # writes at once and monopolize the USB serial endpoint.
                    for _ in range(2):
                        with self.lock:
                            ticket=self.pending.popleft() if self.pending else None
                        if ticket is None:break
                        try:
                            if ticket.key is None and time.monotonic()-ticket.created>1:
                                ticket.done.set();continue
                            try:count=port.write(ticket.data)
                            except Exception as error:
                                print('Serial write failure family={} queued={} error={}'.format(
                                    ticket.data.split(b':',1)[0].decode('ascii',errors='replace').strip(),
                                    len(self.pending),error),flush=True)
                                raise
                            if count!=len(ticket.data):raise OSError('Partial serial line write')
                            ticket.success=True
                        finally:ticket.done.set()
                    data=port.read(min(4096,max(1,port.in_waiting)))
                    for byte in data:
                        if byte==10:
                            if not discard and buf:
                                try:self.on_line(buf.decode('utf-8',errors='replace').strip())
                                except Exception as error:print('Serial command handler: {}'.format(error),flush=True)
                            buf.clear();discard=False
                        elif not discard:
                            if len(buf)>=16384:buf.clear();discard=True
                            else:buf.append(byte)
                    self.on_tick()
            except Exception as error:
                recovery_error=error
                print('Serial reconnect: {}'.format(error),flush=True)
            finally:
                self._disconnect()
                if port is not None:
                    if recovery_error is not None:
                        # A timed-out tty write leaves bytes queued in the
                        # driver. Linux close() then waits about a second for
                        # that dead output to drain. Discard it first; state
                        # is replayed after reopening the port.
                        flush_started=time.monotonic()
                        try:port.reset_output_buffer()
                        except Exception as error:
                            print('Serial recovery flush error={}'.format(error),flush=True)
                        print('Serial recovery flush_ms={:.1f}'.format(
                            (time.monotonic()-flush_started)*1000),flush=True)
                    close_started=time.monotonic()
                    try:port.close()
                    except OSError:pass
                    if recovery_error is not None:
                        print('Serial recovery close_ms={:.1f}'.format(
                            (time.monotonic()-close_started)*1000),flush=True)
            # A write timeout often leaves the USB device present. Reopen it
            # promptly so FFT delivery is interrupted for tens of ms rather
            # than the full one-second device-absence retry. If the reopen
            # fails, the next pass uses the normal one-second interval.
            self.stop.wait(.1 if port is not None else self.retry)

    def close(self):
        self.stop.set();self.thread.join(timeout=2)
        if self.thread.is_alive():raise RuntimeError('Serial owner did not stop')
