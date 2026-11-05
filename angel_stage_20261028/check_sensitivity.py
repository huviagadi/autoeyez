import time
from pathlib import Path
import numpy as np
from angel_shape import DEFAULT_SHAPE
from gpu_shader_sources import GpuShaderSource
shader=GpuShaderSource('ANGEL',Path('/tmp/angel_v2/angel.glsl').read_text(),360,288,output_width=720,output_height=576)
def frame(shape):
    shader.angel_shape=shape
    time.sleep(0.019)
    shader.render_texture(time_override=1.5,controls=(0.5,0.5,0.5))
    shader.ctx.finish()
    return np.frombuffer(shader.output_fbo.read(components=3,alignment=1),dtype=np.uint8).astype('f4')
base=frame(list(DEFAULT_SHAPE))
for idx in range(24):
    shape=list(DEFAULT_SHAPE);shape[idx]=0; low=frame(shape)
    shape[idx]=127; high=frame(shape)
    print(idx, round(float(np.abs(base-low).mean()),2), round(float(np.abs(base-high).mean()),2),round(float(np.abs(low-high).mean()),2))
