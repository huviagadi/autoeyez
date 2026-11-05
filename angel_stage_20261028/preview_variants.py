import time
from pathlib import Path
import cv2
import numpy as np
from angel_shape import DEFAULT_SHAPE
from gpu_shader_sources import GpuShaderSource

source = Path('/tmp/angel_v2/angel.glsl').read_text()
shader = GpuShaderSource('ANGEL', source, 360, 288, output_width=720, output_height=576)
variants = {
    'default': list(DEFAULT_SHAPE),
    'wide': [20,90,114,127,118,125,120,80, 64,16,12,110,5,107,12,76,
             64,72,50,105,120,100,110,0],
    'compact': [85,26,17,25,9,13,15,10, 18,112,113,33,112,19,112,10,
                83,40,118,18,14,10,10,110],
}
images = []
for name, shape in variants.items():
    shader.angel_shape = shape
    time.sleep(0.025)
    shader.render_texture(time_override=1.5, controls=(0.5, 0.5, 0.5))
    shader.ctx.finish()
    rgb = np.frombuffer(shader.output_fbo.read(components=3, alignment=1), dtype=np.uint8).reshape(576,720,3)[::-1].copy()
    cv2.imwrite('/tmp/angel_v2/' + name + '.png', cv2.cvtColor(rgb,cv2.COLOR_RGB2BGR))
    images.append(rgb)
    print(name, 'lit', int(np.count_nonzero(rgb > 60)))
print('wide difference', float(np.abs(images[0].astype('f4')-images[1]).mean()))
print('compact difference', float(np.abs(images[0].astype('f4')-images[2]).mean()))
