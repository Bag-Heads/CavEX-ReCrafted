"""Offline developer tool: capture synthetic Nether texel IDs and shading.

Requires a locally built PC engine; never shipped in the importer bundle.
The default uses the current PC title bake. --legacy-57d-engine reproduces
57h's captures with the retained 57d engine used for the shipped scene art.
Run while holding the project build lock. The output directory must be new.
Then run encode_nether_map.py and compare the resulting composer with engine
renders before updating its pinned hash. No user artwork is read here.
"""
import argparse
from pathlib import Path
import json
import shutil
import os
import subprocess
import time
import hashlib
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--engine', required=True, type=Path)
parser.add_argument('--out', required=True, type=Path)
parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument('--legacy-57d-engine', action='store_true')
args = parser.parse_args()
R = args.repo.resolve()
J = args.out.resolve()
J.mkdir(parents=True, exist_ok=False)
W = J / 'capture'
W.mkdir()
for name in ('assets', 'resources'):
    shutil.copytree(R / name, W / name)
P = W / 'pack'
P.mkdir()
exe = args.engine.resolve()
config = {'paths': {'texturepack': str(P)},
          'debug': {'overlay': 'off', 'hw_overlay': 0},
          'experimental': {'local_splitscreen': 0}}
if args.legacy_57d_engine:
    config['debug'].update({'backdrop_bake_selftest': 1, 'nether_bake': 1,
                           'nether_seed': 42, 'nether_x': -60, 'nether_y': 36,
                           'nether_z': 100, 'nether_yaw': 90, 'nether_pitch': 78})
(W / 'config.json').write_text(json.dumps(config))
shader = (R / 'resources/fragment.shader').read_text()


def capture(name):
    cache = P / '.cavewii'
    if cache.exists():
        shutil.rmtree(cache)  # owned private capture directory, never a player pack
    out = (W / 'backdrop_bake_selftest.png' if args.legacy_57d_engine else
           cache / 'backdrop/nether-background.png')
    out.unlink(missing_ok=True)
    ready = out if args.legacy_57d_engine else out.with_suffix('.id')
    env = os.environ.copy()
    env['CAVEWII_HEADLESS'] = '1'
    env['PATH'] = str(exe.parent) + os.pathsep + env.get('PATH', '')
    startup = None
    if os.name == 'nt':
        startup = subprocess.STARTUPINFO()
        startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
    with (J / (name + '.log')).open('wb') as log:
        process = subprocess.Popen([str(exe)], cwd=W, env=env, stdout=log,
                                   stderr=subprocess.STDOUT, startupinfo=startup)
        try:
            deadline = time.monotonic() + 120
            while not ready.exists():
                if process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError('Capture failed: ' + name)
                time.sleep(0.05)
            time.sleep(0.2)
            with Image.open(out) as image:
                image.convert('RGB').save(J / (name + '.png'))
        finally:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=10)
    print(name, flush=True)


for name, value in (('black', 0), ('white', 255)):
    for atlas in ('terrain', 'anim'):
        Image.new('RGBA', (256, 256), (value, value, value, 255)).save(P / (atlas + '.png'))
    capture(name)
for sheet, atlas in enumerate(('terrain', 'anim')):
    image = Image.new('RGBA', (256, 256))
    image.putdata([(x, y, sheet, 255) for y in range(256) for x in range(256)])
    image.save(P / (atlas + '.png'))
(W / 'resources/fragment.shader').write_text(shader.replace(
    'gl_FragColor = vec4(mix(frag.rgb, fog_color, v_fog), frag.a);',
    'gl_FragColor = tex_color;'))
capture('ids')
(W / 'resources/fragment.shader').write_text(shader)
(J / 'capture-inputs.json').write_text(json.dumps({
    'engine_sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
    'shader_sha256': hashlib.sha256(shader.encode()).hexdigest(), 'config': config}, indent=2))

# The two cutout geometries visible in this recipe: brown mushrooms and fire.
# Lava is the 18px-pitch animation cell with content x=39..54; its frame is 0.
# Peeling removes sampled IDs, so coincident mushroom faces are retained.
# Fire's depth prepass blocks farther animation fragments even at alpha zero;
# preserve that depth while revealing coplanar fire and the opaque pass below.
def cutout(pixel):
    x, y, sheet = pixel
    return ((sheet == 0 and (x // 16, y // 16) == (12, 1)) or
            (sheet == 1 and not 39 <= x <= 54))


excluded, fire_depth, layers = {}, {}, []
active = set(range(1024 * 576))
output_statement = 'gl_FragColor = vec4(mix(frag.rgb, fog_color, v_fog), frag.a);'
for layer in range(12):
    prefix = f'layer{layer}'
    code = 'bool skipSample(vec2 p, vec3 id, float z) {\n'
    for (x, y), ids in sorted(excluded.items()):
        code += (f'if (p.x >= {x}.0 && p.x < {x+1}.0 && '
                 f'p.y >= {y}.0 && p.y < {y+1}.0) {{\n')
        if (x, y) in fire_depth:
            code += f'if (id.b > 0.5 && z > {fire_depth[x,y]:.10f}) return true;\n'
        for u, v, sheet in sorted(ids):
            code += (f'if (abs(id.r-{u}.0)<0.1 && abs(id.g-{v}.0)<0.1 '
                     f'&& abs(id.b-{sheet}.0)<0.1) return true;\n')
        code += '}\n'
    code += 'return false; }\n'
    base = shader.replace('void main() {', code + '\nvoid main() {')
    needle = 'float v_fog = 0.0;'
    base = base.replace(needle, 'if(enable_texture && skipSample(gl_FragCoord.xy, '
                        'tex_color.rgb*255.0, gl_FragCoord.z)) discard;\n' + needle)
    for mode in ('ids', 'depth', 'black', 'white'):
        if layer == 0 and mode != 'depth':
            shutil.copyfile(J / (mode + '.png'), J / (prefix + '-' + mode + '.png'))
            continue
        fragment = base
        if mode in ('black', 'white'):
            color = '0.0,0.0,0.0,1.0' if mode == 'black' else '1.0'
            fragment = fragment.replace(needle, 'if(enable_texture) tex_color=vec4('
                                        + color + ');\n' + needle)
        if mode == 'ids':
            fragment = fragment.replace(output_statement, 'gl_FragColor = tex_color;')
        if mode == 'depth':
            fragment = fragment.replace(output_statement,
                'float d = floor(gl_FragCoord.z * 16777215.0); '
                'gl_FragColor = vec4(mod(d,256.0),mod(floor(d/256.0),256.0),'
                'floor(d/65536.0),255.0)/255.0;')
        (W / 'resources/fragment.shader').write_text(fragment)
        capture(prefix + '-' + mode)
    with Image.open(J / (prefix + '-ids.png')) as image:
        ids = list(image.convert('RGB').getdata())
    with Image.open(J / (prefix + '-depth.png')) as image:
        depth = list(image.convert('RGB').getdata())
    next_active = {i for i in active if cutout(ids[i])}
    print('layer', layer, 'active', len(active), 'cutouts', len(next_active), flush=True)
    layers.append({'prefix': prefix, 'active': sorted(active) if layer else None})
    if not next_active:
        break
    for i in next_active:
        # backdrop_bake_nearest_src uses pixel centres; GL's Y origin is below.
        sx = (2 * (i % 1024) + 1) * 640 // 2048
        sy = 479 - (96 + (2 * (i // 1024) + 1) * 360 // 1152)
        excluded.setdefault((sx, sy), set()).add(ids[i])
        if ids[i][2] == 1:
            d = depth[i]
            fire_depth.setdefault((sx, sy),
                (d[0] + d[1] * 256 + d[2] * 65536) / 16777215 + 0.0000001)
    active = next_active
else:
    raise RuntimeError('Peeling did not terminate; no truncated map allowed')
(J / 'layers.json').write_text(json.dumps(layers))
(W / 'resources/fragment.shader').write_text(shader)
