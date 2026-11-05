#!/usr/bin/env python3
import re

# Read the file
with open('/home/admin/video_player.py', 'r') as f:
    content = f.read()

# New seek_to function that uses buffer
new_seek_to = '''    def seek_to(self, position):
        """
        Seek to a position in the clip (0.0 to 1.0).

        Uses frame buffer for instant seeking when available,
        falls back to ffmpeg restart otherwise.
        """
        # Use frame buffer if available for instant seeking
        if self.buffer_ready and self.buffer_frame_count > 0:
            frame_idx = int(position * (self.buffer_frame_count - 1))
            frame_idx = max(0, min(frame_idx, self.buffer_frame_count - 1))
            self.playback_frame_idx = frame_idx
            self.seek_active = True
            # Update current_frame immediately
            with self.frame_lock:
                self.current_frame = self.frame_buffer[frame_idx].copy()
            print("SEEK (buffer): {:.1f}% -> frame {}/{}".format(
                position * 100, frame_idx, self.buffer_frame_count), flush=True)
        else:
            # Fall back to ffmpeg restart
            print("SEEK (ffmpeg): {:.1f}%".format(position * 100), flush=True)
            seek_time = position * self.duration
            self.play_current(seek=seek_time)

'''

# Find and replace the seek_to function
pattern = r'    def seek_to\(self, position\):.*?(?=\n    def play_current)'
content = re.sub(pattern, new_seek_to, content, flags=re.DOTALL)

with open('/home/admin/video_player.py', 'w') as f:
    f.write(content)

print('Patched seek_to function')
