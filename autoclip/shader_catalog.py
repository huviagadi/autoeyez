"""Shader source indices shared with the Teensy source menus."""

from pathlib import Path


NEW_SHADER_SPECS = (
    (16, 'ASHANOHA', 'ashanoha.glsl'),
    (17, 'TILES', 'tiles.glsl'),
    (18, 'CURLESQUE', 'curlesque.glsl'),
    (19, 'SEIGAIHA', 'seigaiha.glsl'),
    (20, 'TRI-PATTERN', 'tri_pattern.glsl'),
    (21, 'EYE OF EZBHAN', 'eye_of_ezbhan.glsl'),
    (22, 'CHROMAWAVE', 'chromawave.glsl'),
    (23, 'ICEBREAKER', 'icebreaker.glsl'),
    (24, 'VOXELSCAPE', 'voxelscape.glsl'),
    (25, 'POLY MANGLE', 'poly_mangle.frag'),
)


def shader_source(filename):
    return (Path(__file__).parent / 'queued_shaders' / filename).read_text()
