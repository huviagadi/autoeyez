from patch_transport import FrameInbox,POOL
#!/usr/bin/env python3
"""
video_player.py — PyAV-based Clip Playback for autoclip
With event-driven frame handoff for reduced latency.
"""

import os
import time
import json
import socket
import select
import threading
from command_ipc import CommandReceiver, PLAYER_COMMAND_SOCKET
import numpy as np
import av
import clip_catalog
from clip_frame_converter import ClipFrameConverter
import trace_runtime

CLIPS_DIR = '/home/admin/clips/'
SEEK_CLIPS_DIR = '/home/admin/clips_seek/'
STATE_FILE = '/tmp/player_state.json'
SEEK_SOCKET = '/tmp/player_seek.sock'

W, H = 720, 576
DEFAULT_CLIP = 'CRYSTALS'
TARGET_FPS = 30
FRAME_TIME = 1.0 / TARGET_FPS

def get_clips():
    return clip_catalog.names()

class VideoPlayer:
    def __init__(self):
        self.clips = get_clips()
        self.index = self._find_default()
        self.paused = False
        self.loop = True
        
        self.frame_converter = ClipFrameConverter(W, H)
        self.current_frame = np.zeros((H, W, 3), dtype=np.uint8)
        self.frame_lock = threading.Lock()
        self.trace_source=trace_runtime.SourceTracker('clips',30) if trace_runtime.ENABLED else None
        self.frame_ready = threading.Event()  # NEW: signal when frame is ready
        
        self.time_pos = 0.0
        self.duration = 0.0
        self.frame_period = 1.0/30
        self.next_frame_time = time.perf_counter()
        self.container = None
        self.stream = None
        
        self.running = True
        self.seek_request = None
        self.seek_decode_target = None
        self.seek_started_at = None
        self.pending_event = None
        self.current_event = None
        self.latency_marker_enabled = os.environ.get('AUTOEYEZ_CLIP_LATENCY_MARKER') == '1'
        self.marker_frames_remaining = 0
        self.marker_value = None
        self.seek_lock = threading.Lock()
        self.stutter_start = 0.0
        self.stutter_lengths = {'normal': 0, 'reverse': 0}
        self.stutter_indices = {'normal': 0, 'reverse': 0}
        self.stutter_last_control = 'normal'
        self.stutter_buffer = []
        self.stutter_release = []
        self.stutter_replaying = False
        # The mixer now replays final output frames at 30 fps. Keep decoding
        # clips normally so release can jump to the current live timeline.
        self.stutter_in_mixer = True
        
        self._init_socket()
        self.commands = CommandReceiver(PLAYER_COMMAND_SOCKET, 'player')
        self._patch_previous=None
        self.patch_inbox=FrameInbox('player',self._prepare_patch,self._apply_patch,self._dispose_patch)
        self.write_state()
    
    def _init_socket(self):
        if os.path.exists(SEEK_SOCKET):
            os.remove(SEEK_SOCKET)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(SEEK_SOCKET)
        self.sock.setblocking(False)
        os.chmod(SEEK_SOCKET, 0o666)
    
    def _find_default(self):
        for i, c in enumerate(self.clips):
            if DEFAULT_CLIP.upper() in c.upper():
                return i
        return 0
    
    def write_state(self):
        try:
            with open(STATE_FILE, 'w') as f:
                json.dump({
                    'index': self.index,
                    'paused': self.paused,
                    'loop': self.loop,
                    'clip': self.clips[self.index] if self.clips else '',
                    'time_pos': self.time_pos,
                    'duration': self.duration,
                    'frame_period': self.frame_period,
                    'stutter': max(self.stutter_lengths.values()),
                    'stutter_controls': self.stutter_lengths
                }, f)
        except:
            pass
    
    def open_clip(self):
        self.seek_decode_target = None
        self.pending_event = None
        with self.frame_lock:
            self.current_event = None
        self.marker_frames_remaining = 0
        self.stutter_lengths = {'normal': 0, 'reverse': 0}
        self.stutter_indices = {'normal': 0, 'reverse': 0}
        self.stutter_buffer = []
        self.stutter_release = []
        self.stutter_replaying = False
        if self.container:
            self.container.close()
            self.container = None
        
        if not self.clips:
            return
        
        source_path = os.path.join(CLIPS_DIR, self.clips[self.index])
        seek_path = os.path.join(SEEK_CLIPS_DIR, self.clips[self.index])
        path = seek_path if os.path.exists(seek_path) else source_path
        try:
            self.container = av.open(path)
            self.stream = self.container.streams.video[0]
            self.stream.thread_type = 'AUTO'
            self.duration = float(self.stream.duration * self.stream.time_base) if self.stream.duration else 0
            self.frame_period = 1.0 / float(self.stream.average_rate) if self.stream.average_rate else 1.0/30
            self.next_frame_time = time.perf_counter()
            self.time_pos = 0.0
            source_kind = 'seek' if path == seek_path else 'original'
            print(f'Opened: {self.clips[self.index]} ({self.duration:.1f}s, {source_kind})', flush=True)
        except Exception as e:
            print(f'Failed to open: {e}', flush=True)
            self.container = None
    
    def seek(self, position, event=None):
        if self.container and self.duration > 0:
            with self.seek_lock:
                self.seek_request = position
                self.seek_started_at = time.perf_counter()
                self.pending_event = event
    
    def _do_seek(self, position):
        target_sec = min(max(0.0, position * self.duration),
                         max(0.0, self.duration - self.frame_period))
        target_pts = int(target_sec / self.stream.time_base)
        try:
            self.container.seek(target_pts, stream=self.stream)
            # seek() lands on the preceding keyframe. Decode forward before
            # displaying a frame so note velocity retains its fine resolution.
            self.seek_decode_target = target_sec
            self.stutter_start = target_sec
            self.stutter_buffer = []
            self.stutter_release = []
            self.stutter_indices = {'normal': 0, 'reverse': 0}
            self.stutter_replaying = False
            print(f'Seek: {position*100:.0f}% -> {target_sec:.2f}s', flush=True)
        except Exception as e:
            print(f'Seek failed: {e}', flush=True)

    def set_stutter(self, value, control='normal'):
        """Set one lane of the shared two-head 30-frame stutter buffer."""
        if control not in self.stutter_lengths:
            return
        value = max(0, min(30, int(value)))
        was_active = any(self.stutter_lengths.values())
        if value == 0:
            released_index = self.stutter_indices[control]
            self.stutter_lengths[control] = 0
            if was_active and not any(self.stutter_lengths.values()):
                if self.stutter_replaying:
                    start = min(released_index, len(self.stutter_buffer))
                    self.stutter_release = self.stutter_buffer[start:]
                self.stutter_buffer = []
                self.stutter_indices = {'normal': 0, 'reverse': 0}
                self.stutter_replaying = False
                print('Stutter off', flush=True)
            else:
                print('Stutter {} lane off'.format(control), flush=True)
            return

        if not was_active:
            self.stutter_start = max(0.0, min(self.time_pos, self.duration))
            self.stutter_buffer = []
            self.stutter_indices = {'normal': 0, 'reverse': 0}
            self.stutter_release = []
            self.stutter_replaying = False
        self.stutter_lengths[control] = value
        self.stutter_last_control = control
        if len(self.stutter_buffer) < max(self.stutter_lengths.values()):
            self.stutter_replaying = False
        print('Stutter {}: start={:.3f}s length={} frames ({:.3f}s)'.format(
            control, self.stutter_start, value, value / TARGET_FPS), flush=True)
    
    def get_trace_frame(self):
        with self.frame_lock:
            self.trace_source.rate=1.0/self.frame_period
            state='paused' if self.paused else ('stutter' if self.stutter_replaying else 'playing')
            return self.current_frame.copy(),self.trace_source.origin(self.current_frame,state)

    def get_frame(self):
        with self.frame_lock:
            return self.current_frame.copy()

    def get_frame_state(self):
        with self.frame_lock:
            return self.current_frame.copy(), self.current_event

    def _stamp_latency_marker(self, frame):
        """Optional 12-cell ID in clip pixels for a receiver draw-loop probe."""
        if self.marker_frames_remaining <= 0 or self.marker_value is None:
            return frame
        # PyAV's RGB view may be read-only. Only the diagnostic frames need a
        # writable copy; normal playback keeps the existing zero-copy path.
        if not frame.flags.writeable:
            frame = frame.copy()
        bits = (1, 0, 1, 1) + tuple((self.marker_value >> shift) & 1
                                      for shift in range(7, -1, -1))
        for index, bit in enumerate(bits):
            frame[0:12, index * 12:(index + 1) * 12] = 255 if bit else 0
        self.marker_frames_remaining -= 1
        return frame

    def _reset_stutter(self):
        self.stutter_lengths = {'normal': 0, 'reverse': 0}
        self.stutter_indices = {'normal': 0, 'reverse': 0}
        self.stutter_buffer = []
        self.stutter_release = []
        self.stutter_replaying = False

    def _handle_seek_datagrams(self):
        """Drain one MIDI burst; cue and following position share one decoder seek."""
        messages = []
        def drain():
            while len(messages) < 64:
                try:
                    messages.append(json.loads(self.sock.recv(512).decode()))
                except BlockingIOError:
                    break
                except (ValueError, UnicodeDecodeError):
                    continue
        drain()
        if messages and (messages[-1].get('action') == 'play_index' or
                         'position' in messages[-1]):
            # Force notes on one beat can arrive as seek-then-G8 or G8-then-
            # seek. Give the companion datagram a few milliseconds to arrive.
            if select.select([self.sock], [], [], 0.005)[0]:
                drain()
        cue = None
        position = None
        event = None
        def flush():
            nonlocal cue, position, event
            if cue is not None:
                command = dict(cue)
                if position is not None:
                    command['position'] = position
                if event is not None:
                    command['event_id'] = event.get('event_id')
                    command['sent_ns'] = event.get('sent_ns')
                self.handle_cmd(command)
            elif position is not None:
                self.seek(position, event)
            cue = position = event = None
        for message in messages:
            action = message.get('action')
            if action == 'play_index':
                # Force can deliver the position note before G8 on the same
                # beat. Keep it: the position wins in either message order.
                cue = message
                if position is None:
                    event = message
            elif 'position' in message and action is None:
                position = message['position']
                event = message
            else:
                flush()
                if action in ('stutter_control', 'stutter'):
                    self.set_stutter(message.get('value', 0),
                                     message.get('control', 'normal'))
                else:
                    self.handle_cmd(message)
        flush()
    
    def wait_frame(self, timeout=0.05):
        """Wait for next frame with timeout. Returns True if frame ready."""
        return self.frame_ready.wait(timeout)
    
    def next(self):
        self.index = (self.index + 1) % len(self.clips) if self.clips else 0
        self.open_clip()
        self.write_state()
    
    def prev(self):
        self.index = (self.index - 1) % len(self.clips) if self.clips else 0
        self.open_clip()
        self.write_state()
    
    def _prepare_patch(self, payload):
        if set(payload)!= {'index','loop'} or type(payload['index']) is not int or type(payload['loop']) is not bool or not 0<=payload['index']<len(self.clips):raise ValueError('invalid player patch')
        before={'index':self.index,'loop':self.loop,'restore':True,'paused':self.paused,'position':self.time_pos/self.duration if self.duration>0 else 0}
        previous=(self.container,self.stream,self.index,self.loop,self.paused,self.time_pos,self.duration,self.frame_period)
        index=payload['index'];source_path=os.path.join(CLIPS_DIR,self.clips[index]);seek_path=os.path.join(SEEK_CLIPS_DIR,self.clips[index])
        path=seek_path if os.path.exists(seek_path) else source_path
        def preload():
            container=av.open(path)
            try:
                stream=container.streams.video[0];stream.thread_type='AUTO'
                # Decode validates actual media, then rewind for ordinary conversion.
                next(container.decode(stream));container.seek(0,stream=stream)
                return dict(payload,container=container,stream=stream,previous=previous),before
            except BaseException:container.close();raise
        return POOL.submit(preload)

    def _apply_patch(self, payload):
        if payload.get('restore'):
            if self._patch_previous is None:
                import concurrent.futures
                source=self._prepare_patch({'index':payload['index'],'loop':payload['loop']});result=concurrent.futures.Future()
                def ready(completed):
                    try:
                        candidate,before=completed.result();candidate['restore_paused']=bool(payload.get('paused',False));candidate['restore_position']=max(0.,min(1.,float(payload.get('position',0))))
                        result.set_result((candidate,before))
                    except BaseException as e:result.set_exception(e)
                source.add_done_callback(ready);return result
            old=self.container
            for key,value in self._patch_previous.items():setattr(self,key,value)
            if self._patch_candidate is not None:self._patch_candidate['container']=None;self._patch_candidate['previous']=None
            self._patch_candidate=None
            if old is not None and old is not self.container:POOL.submit(old.close)
        else:
            payload['previous']={key:getattr(self,key) for key in ('container','stream','index','loop','paused','time_pos','duration','frame_period','seek_decode_target','seek_request','seek_started_at','stutter_start','stutter_lengths','stutter_indices','stutter_buffer','stutter_release','stutter_replaying','stutter_last_control','current_frame')}
            self._patch_candidate=payload
            self._patch_previous=payload['previous']
            self.container=payload['container'];self.stream=payload['stream'];self.index=payload['index'];self.loop=payload['loop'];self.paused=bool(payload.get('restore_paused',False))
            self.duration=float(self.stream.duration*self.stream.time_base) if self.stream.duration else 0
            self.frame_period=1.0/float(self.stream.average_rate) if self.stream.average_rate else 1.0/30
            self.time_pos=0.0
        if not payload.get('restore'):
            self.seek_decode_target=None;self.seek_request=None
            self.stutter_lengths={'normal':0,'reverse':0};self.stutter_indices={'normal':0,'reverse':0};self.stutter_buffer=[];self.stutter_release=[];self.stutter_replaying=False
        if 'restore_position' in payload:self.seek(payload['restore_position'])
        self.next_frame_time=time.perf_counter();self.write_state()

    def _dispose_patch(self,payload):
        candidate=payload.get('container')
        if candidate is not None and candidate is not self.container:POOL.submit(candidate.close)
        previous=payload.get('previous')
        previous_container=previous.get('container') if isinstance(previous,dict) else previous[0] if previous else None
        if previous_container is not None and previous_container is not self.container:POOL.submit(previous_container.close)
        payload['container']=None;payload['previous']=None
        self._patch_candidate=None
        self._patch_previous=None

    def handle_cmd(self, cmd):
        action = cmd.get('action')
        if action == 'next':
            self.next()
        elif action == 'prev':
            self.prev()
        elif action == 'play_index':
            index = cmd.get('index', 0) % len(self.clips) if self.clips else 0
            same_clip = index == self.index and self.container is not None
            self.index = index
            if same_clip:
                self._reset_stutter()
            else:
                self.open_clip()
            event = {key: cmd[key] for key in ('event_id', 'sent_ns', 'sent_wall_ns')
                     if key in cmd}
            self.seek(cmd.get('position', 0.0), event or None)
            self.write_state()
        elif action == 'play_loop':
            # Restore the clip and loop setting together when loading a patch.
            self.index = cmd.get('index', 0) % len(self.clips) if self.clips else 0
            self.loop = bool(cmd.get('loop', False))
            self.paused = False
            self.open_clip()
            self.write_state()
        elif action == 'loop_on':
            self.loop = True
            self.write_state()
        elif action == 'loop_off':
            self.loop = False
            self.write_state()
        elif action == 'pause':
            self.paused = True
            self.write_state()
        elif action == 'play':
            self.paused = False
            self.write_state()
        elif action == 'cue_preview':
            # Hold the requested frame for keypad cue editing. The decode loop
            # is allowed to land one frame while paused.
            self.paused = True
            self.seek(cmd.get('position', 0.0))
            self.write_state()
    
    def run(self):
        self.open_clip()
        last_state_write = 0
        last_catalog_sync = 0
        frame_count = 0
        fps_start = time.time()
        
        while self.running:
            now = time.time()
            if now - last_catalog_sync >= 5:
                refreshed = get_clips()
                if refreshed != self.clips:
                    self.clips = refreshed
                    self.write_state()
                last_catalog_sync = now
            
            self._handle_seek_datagrams()
            
            # Actions remain ordered and apply before decode, as before. Bound
            # each batch so an exceptional burst cannot monopolize the loop.
            for cmd in self.commands.drain(limit=8):
                self.handle_cmd(cmd)

            self.patch_inbox.poll()

            # Handle seek request
            did_seek = False
            with self.seek_lock:
                if self.seek_request is not None:
                    self._do_seek(self.seek_request)
                    self.seek_request = None
                    did_seek = True

            # A MIDI wakeup only bypasses source pacing for an actual seek.
            # Other datagrams (for example stutter controls) must not make the
            # decoder run faster than the clip's own frame rate.
            if (self.container and not did_seek and not self.paused and
                    self.seek_decode_target is None):
                wait = self.next_frame_time - time.perf_counter()
                if wait > 0:
                    select.select([self.sock], [], [], wait)
                    continue
            
            # Decode next frame if not paused
            if (not self.paused or self.seek_decode_target is not None) and self.container:
                replay_refs = ()
                replay_arr = None
                replay_pos = None
                if not self.stutter_in_mixer and self.stutter_release:
                    replay_arr = self.stutter_release.pop(0)
                    if self.trace_source: replay_refs=self.trace_source.references(replay_arr)
                    replay_pos = self.time_pos + self.frame_period
                elif (not self.stutter_in_mixer and any(self.stutter_lengths.values()) and
                      len(self.stutter_buffer) >= max(self.stutter_lengths.values())):
                    self.stutter_replaying = True
                    heads = []
                    positions = {}
                    for control, length in self.stutter_lengths.items():
                        if length <= 0:
                            continue
                        index = self.stutter_indices[control] % length
                        positions[control] = index
                        heads.append(self.stutter_buffer[index])
                        self.stutter_indices[control] = (index + 1) % length
                    replay_arr = heads[0]
                    if self.trace_source: replay_refs=tuple(ref for head in heads for ref in self.trace_source.references(head))
                    if len(heads) == 2:
                        replay_arr = ((heads[0].astype(np.uint16) +
                                       heads[1].astype(np.uint16)) // 2).astype(np.uint8)
                    position_index = positions.get(
                        self.stutter_last_control, next(iter(positions.values())))
                    replay_pos = self.stutter_start + position_index / TARGET_FPS

                if replay_arr is not None:
                    with self.frame_lock:
                        self.current_frame = replay_arr
                        if self.trace_source: self.trace_source.remember(self.current_frame,replay_refs)
                    self.frame_ready.set()
                    self.time_pos = replay_pos
                    frame_count += 1
                    self.next_frame_time += self.frame_period
                    if self.next_frame_time - time.perf_counter() < -0.1:
                        self.next_frame_time = time.perf_counter()
                    time.sleep(0.001)
                    continue

                try:
                    for frame in self.container.decode(self.stream):
                        if self.seek_decode_target is not None:
                            frame_time = (float(frame.pts * self.stream.time_base)
                                          if frame.pts is not None else None)
                            if (frame_time is not None and
                                    frame_time + self.frame_period * 0.5 < self.seek_decode_target):
                                continue
                            self.seek_decode_target = None
                            self.next_frame_time = time.perf_counter()
                            if self.seek_started_at is not None:
                                seek_ms = (time.perf_counter() - self.seek_started_at) * 1000.0
                                print(f'Seek landed: {seek_ms:.1f}ms', flush=True)
                                self.seek_started_at = None
                        arr = self.frame_converter.convert(frame)
                        
                        event = self.pending_event
                        if event:
                            published_ns = time.monotonic_ns()
                            event = dict(event, published_ns=published_ns)
                            if self.latency_marker_enabled:
                                try:
                                    self.marker_value = int(str(event['event_id']).rsplit('-', 1)[1]) & 255
                                    self.marker_frames_remaining = 4
                                    print('CLIP_LATENCY stage=marker event={} code={} sent_wall_ns={}'.format(
                                        event['event_id'], self.marker_value,
                                        event.get('sent_wall_ns')), flush=True)
                                except (KeyError, ValueError):
                                    self.marker_frames_remaining = 0
                            sent_ns = event.get('sent_ns')
                            if isinstance(sent_ns, int) and sent_ns <= published_ns:
                                print('CLIP_LATENCY stage=publish event={} elapsed_ms={:.1f}'.format(
                                    event.get('event_id'), (published_ns-sent_ns)/1e6), flush=True)
                            self.pending_event = None
                        arr = self._stamp_latency_marker(arr)
                        with self.frame_lock:
                            self.current_frame = arr
                            if event:
                                self.current_event = event
                            if self.trace_source: self.trace_source.publish(self.current_frame)
                        
                        # Signal frame ready
                        self.frame_ready.set()
                        
                        self.time_pos = float(frame.pts * self.stream.time_base) if frame.pts else self.time_pos
                        frame_count += 1
                        target_frames = max(self.stutter_lengths.values())
                        if (not self.stutter_in_mixer and target_frames > 0 and
                                len(self.stutter_buffer) < target_frames):
                            self.stutter_buffer.append(arr.copy())
                            if self.trace_source: self.trace_source.remember(self.stutter_buffer[-1],self.trace_source.references(arr))
                            if len(self.stutter_buffer) == target_frames:
                                self.stutter_indices = {'normal': 0, 'reverse': 0}
                        
                        # Frame rate pacing
                        self.next_frame_time += self.frame_period
                        if self.next_frame_time - time.perf_counter() < -0.1:
                            self.next_frame_time = time.perf_counter()
                        break
                    
                except av.error.EOFError:
                    self.seek_decode_target = None
                    if self.loop:
                        self.container.seek(0, stream=self.stream)
                        self.time_pos = 0
                    else:
                        self.next()
                except StopIteration:
                    self.seek_decode_target = None
                    if self.loop:
                        self.container.seek(0, stream=self.stream)
                        self.time_pos = 0
                    else:
                        self.next()
                except Exception as e:
                    print(f'Decode error: {e}', flush=True)
            
            # Write state periodically
            if now - last_state_write >= 1.0:
                self.write_state()
                last_state_write = now
                elapsed = now - fps_start
                if elapsed > 0:
                    fps = frame_count / elapsed
                    print(f'FPS: {fps:.1f}', flush=True)
                frame_count = 0
                fps_start = now
            
            # Minimal sleep when not decoding
            time.sleep(0.001)
    
    def stop(self):
        self.running = False
        if self.container:
            self.container.close()

if __name__ == '__main__':
    player = VideoPlayer()
    try:
        player.run()
    except KeyboardInterrupt:
        player.stop()
