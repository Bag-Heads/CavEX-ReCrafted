"""Independent Python IEEE/network-order oracle for the production movement encoder."""
import random
import struct
import subprocess
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
rng = random.Random(9122026)
cases = [(1., 2., 3., 4., 5., 6., True), (-0., 0., -0., 0., -0., 0., False)]
for _ in range(1024):
    cases.append(tuple(rng.uniform(-3e7, 3e7) for _ in range(4)) +
                 tuple(rng.uniform(-360, 360) for _ in range(2)) + (bool(rng.randrange(2)),))
rows = []
for values in cases:
    wire = b'\x0d' + struct.pack('>ddddff?', *values)
    rows.append('{' + ','.join(v.hex() for v in values[:6]) + ',' + str(int(values[6])) +
                ',{' + ','.join(str(b) for b in wire) + '}}')
code = r'''
#include <assert.h>
#include <string.h>
#include "network/mp_codec.h"
static const struct { double x,y,s,z;float yaw,pitch;bool ground;unsigned char bytes[42]; } cases[]={CASES};
int main(void) {
 struct { unsigned char before[16];struct mp_frame frame;unsigned char after[16]; } guard;
 memset(&guard,0xa5,sizeof guard);
 for(unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
  assert(mp_encode_player_look_move(&guard.frame,cases[i].x,cases[i].y,cases[i].s,
   cases[i].z,cases[i].yaw,cases[i].pitch,cases[i].ground));
  assert(guard.frame.length==42 && memcmp(guard.frame.data,cases[i].bytes,42)==0);
  for(unsigned j=0;j<16;j++)assert(guard.before[j]==0xa5 && guard.after[j]==0xa5);
 }
}
'''.replace('CASES', ',\n'.join(rows))
with tempfile.TemporaryDirectory() as temp:
    source = Path(temp)/'wire.c'
    exe = source.with_suffix('.exe')
    source.write_text(code)
    subprocess.run(['C:/msys64/ucrt64/bin/gcc.exe','-O3','-flto','-UNDEBUG',
                    '-I'+str(root/'source'),str(source),str(root/'source/network/mp_codec.c'),
                    '-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print(f'PASS: {len(cases)} movement packets match independent network-order IEEE bytes, including signed zero; guards intact')
