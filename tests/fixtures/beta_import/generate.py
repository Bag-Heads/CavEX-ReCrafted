"""Original Beta 1.7.3 save methods via JNI; no transformed reference classes."""
import argparse
import hashlib
import json
import shutil
import struct
import sys
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tests/oracle/lighting/g9_option_b/capture'))
from beta_jni import JVM

PIN = 'ebb4ba06f6288c147865ff2d3a76bf9034f07dc3f2792335fe22dd41d3c8d7b7'


def nbt(data):
    """Independent NBT decoder, used only to extract the actual saved arrays."""
    pos = 0
    def take(n):
        nonlocal pos
        out = data[pos:pos+n]
        assert len(out) == n
        pos += n
        return out
    def number(fmt):
        return struct.unpack('>' + fmt, take(struct.calcsize(fmt)))[0]
    def text():
        return take(number('H')).decode('utf8')
    def payload(t):
        if t in (1, 2, 3, 4, 5, 6):
            return number({1:'b', 2:'h', 3:'i', 4:'q', 5:'f', 6:'d'}[t])
        if t == 7:
            return take(number('i'))
        if t == 8:
            return text()
        if t == 9:
            kind, count = number('B'), number('i')
            return [payload(kind) for _ in range(count)]
        if t == 10:
            out = {}
            while (kind := number('B')):
                name = text()
                out[name] = payload(kind)
            return out
        raise ValueError(t)
    kind = number('B')
    text()
    out = payload(kind)
    assert pos == len(data)
    return out


def run(args):
    assert hashlib.sha256(args.jar.read_bytes()).hexdigest() == PIN
    args.work.mkdir(parents=True, exist_ok=False)
    classes = args.work / 'classes'
    with zipfile.ZipFile(args.jar) as archive:
        archive.extractall(classes)
    with zipfile.ZipFile(args.resources) as archive:
        for name in archive.namelist():
            if name.startswith(('lang/', 'achievement/')) and not name.endswith('/'):
                archive.extract(name, classes)
    j = JVM(args.jvm, classes.resolve())
    save = j.new('net/minecraft/src/SaveOldDir', '(Ljava/io/File;Ljava/lang/String;Z)V',
                 ('l', j.new('java/io/File', '(Ljava/lang/String;)V', ('l', j.string(args.work)))),
                 ('l', j.string('world')), ('z', 0))
    info = j.new('net/minecraft/src/WorldInfo', '(JLjava/lang/String;)V', ('j', 4242), ('l', j.string('L2')))
    j.call(info, 'setSpawnPosition', '(III)V', ('i', 40), ('i', 70), ('i', 40))
    j.call(save, 'func_22094_a', '(Lnet/minecraft/src/WorldInfo;)V', ('l', info))
    worlds = []
    for dim in (0, -1):
        world = j.new('net/minecraft/src/WorldServer',
            '(Lnet/minecraft/server/MinecraftServer;Lnet/minecraft/src/ISaveHandler;Ljava/lang/String;IJ)V',
            ('l', None), ('l', save), ('l', j.string('L2')), ('i', dim), ('j', 4242))
        provider = j.field(world, 'chunkProviderServer', 'Lnet/minecraft/src/ChunkProviderServer;')
        j.call(j.field(world, 'rand', 'Ljava/util/Random;'), 'setSeed', '(J)V', ('j', 4242))
        for x in range(4):
            for z in range(4):
                j.call(provider, 'loadChunk', '(II)Lnet/minecraft/src/Chunk;', ('i', x), ('i', z))
        # Stable sources installed by real setters, including a Nether plain-setter
        # height/Sky witness (Chunk.setBlockID is intentionally unguarded).
        for x, y, z, block in ((8,75,8,89), (9,75,8,50), (10,75,8,11)):
            j.call(world, 'setBlock', '(IIII)Z', ('i', x), ('i', y), ('i', z), ('i', block))
        if dim == -1:
            # Initialize the plain-setter height then remove the top roof block.
            # Original Beta writes stored Sky=15 above the resulting height.
            for block in (89, 0):
                assert j.call(world, 'setBlock', '(IIII)Z', ('i', 8), ('i', 127), ('i', 8), ('i', block))
        while j.call(world, 'func_6156_d', '()Z'):
            pass
        j.call(world, 'saveWorld', '(ZLnet/minecraft/src/IProgressUpdate;)V', ('z', 1), ('l', None))
        worlds.append((dim, world, provider))
    j.call(save, 'func_22093_e', '()V')
    fixture = Path(__file__).parent
    shutil.copytree(args.work / 'world', fixture / 'world', dirs_exist_ok=True)
    golden = bytearray()
    chunks = []
    for dim in (0, -1):
        path = fixture / 'world' / ('DIM-1' if dim else '') / 'region/r.0.0.mcr'
        region = path.read_bytes()
        for x in range(4):
            for z in range(4):
                offset = int.from_bytes(region[4*(x+32*z):4*(x+32*z)+3], 'big') * 4096
                size = int.from_bytes(region[offset:offset+4], 'big')
                assert region[offset+4] == 2
                level = nbt(zlib.decompress(region[offset+5:offset+4+size]))['Level']
                assert (level['xPos'], level['zPos']) == (x, z)
                image = level['SkyLight'] + level['BlockLight'] + level['HeightMap']
                assert len(image) == 33024
                golden.extend(image)
                chunks.append({'dimension': dim, 'x': x, 'z': z, 'populated': level['TerrainPopulated'],
                               'nonzero_sky_bytes': sum(b != 0 for b in level['SkyLight'])})
    (fixture / 'stored-light.bin').write_bytes(golden)
    # Same absent east-neighbour request used by the C test. Freeze all resident
    # light arrays after the original provider's population and light drain.
    seams = bytearray()
    for d, (dim, _, _) in enumerate(worlds):
        # Reload the real saved files through the original JAR as well. The seam
        # oracle therefore starts from exactly the disk image CaveWii imports.
        world = j.new('net/minecraft/src/WorldServer',
            '(Lnet/minecraft/server/MinecraftServer;Lnet/minecraft/src/ISaveHandler;Ljava/lang/String;IJ)V',
            ('l', None), ('l', save), ('l', j.string('L2')), ('i', dim), ('j', 4242))
        provider = j.field(world, 'chunkProviderServer', 'Lnet/minecraft/src/ChunkProviderServer;')
        j.call(j.field(world, 'rand', 'Ljava/util/Random;'), 'setSeed', '(J)V', ('j', 4242))
        for x in range(4):
            for z in range(4):
                chunk = j.call(provider, 'loadChunk', '(II)Lnet/minecraft/src/Chunk;', ('i', x), ('i', z))
                index = (d*16 + x*4 + z)*33024
                assert j.snapshot(chunk)[49152:] == golden[index:index+33024], (dim,x,z)
        for z in range(4):
            j.call(provider, 'loadChunk', '(II)Lnet/minecraft/src/Chunk;', ('i', 4), ('i', z))
        while j.call(world, 'func_6156_d', '()Z'):
            pass
        for x in range(5):
            for z in range(4):
                chunk = j.call(provider, 'provideChunk', '(II)Lnet/minecraft/src/Chunk;', ('i', x), ('i', z))
                seams.extend(j.snapshot(chunk)[49152:])
    (fixture / 'east-light.bin').write_bytes(seams)
    # session.lock is not part of an import fixture.
    (fixture / 'world/session.lock').unlink(missing_ok=True)
    files = {str(p.relative_to(fixture)): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in fixture.rglob('*') if p.is_file() and p.name != 'provenance.json'}
    (fixture / 'provenance.json').write_text(json.dumps({'jar_sha256': PIN, 'seed': 4242,
        'world_rand_seed': 4242, 'chunks': chunks, 'files': files,
        'command': 'python tests/fixtures/beta_import/generate.py --work ' + str(args.work),
        'scope': 'original WorldServer/ChunkProviderServer/SaveOldDir methods, no normal server main loop; original JAR reload confirms all saved arrays; east seam loads x=4,z=0..3 then func_6156_d drain'}, indent=2))
    print('Saved original Beta .mcr: 32 chunks, stored arrays and east-neighbour oracle')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, required=True)
    p.add_argument('--jar', type=Path, default=Path('C:/tmp/mcb173-server-decompile/minecraft_server.jar'))
    p.add_argument('--resources', type=Path, default=Path('C:/tmp/kb-oracle-p1/vanilla.jar'))
    p.add_argument('--jvm', type=Path, default=Path('C:/Program Files/Java/jre1.8.0_491/bin/server/jvm.dll'))
    run(p.parse_args())
