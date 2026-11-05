#!/usr/bin/env python3
import re

# Read the file
with open('/home/admin/video_player.py', 'r') as f:
    content = f.read()

# New seek_to function that handles partial buffers correctly
new_seek_to = '''    def seek_to(self, position):
        """
        Seek to a position in the clip (0.0 to 1.0).

        Uses frame buffer for instant seeking when the target frame
        is within the buffered range, falls back to ffmpeg otherwise.
        """
        # Calculate target frame in the full clip (assuming 30fps)
        total_frames = int(self.duration * 30) if self.duration > 0 else 1
        target_frame = int(position * total_frames)

        # Use frame buffer if target is within buffered range
        if self.buffer_ready and self.buffer_frame_count > 0 and target_frame < self.buffer_frame_count:
            frame_idx = max(0, min(target_frame, self.buffer_frame_count - 1))
            self.playback_frame_idx = frame_idx
            self.seek_active = True
            # Update current_frame immediately
            with self.frame_lock:
                self.current_frame = self.frame_buffer[frame_idx].copy()
            print("SEEK (buffer): {:.1f}% -> frame {}/{}".format(
                position * 100, frame_idx, self.buffer_frame_count), flush=True)
        else:
            # Fall back to ffmpeg restart
            print("SEEK (ffmpeg): {:.1f}% target_frame={} buffer={}".format(
                position * 100, target_frame, self.buffer_frame_count), flush=True)
            seek_time = position * self.duration
            self.play_current(seek=seek_time)

'''

# Find and replace the seek_to function
pattern = r'    def seek_to\(self, position\):.*?(?=\n    def play_current)'
content = re.sub(pattern, new_seek_to, content, flags=re.DOTALL)

with open('/home/admin/video_player.py', 'w') as f:
    f.write(content)

print('Patched seek_to function v2')
