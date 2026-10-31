# Original Beta 1.7.3 imported lighting fixture

32 real saved chunks: x,z = 0..3, seed 4242, Overworld and Nether.
`world/region/r.0.0.mcr`, `world/DIM-1/region/r.0.0.mcr`, and `level.dat`
are emitted by the original server JAR's SaveOldDir/WorldServer save methods.
No CaveWii-generated blocks or light serve as reference expectations.

Pinned JAR SHA256:
`ebb4ba06f6288c147865ff2d3a76bf9034f07dc3f2792335fe22dd41d3c8d7b7`.
See `provenance.json` for all file hashes, per-chunk population flags, and command.
The generator imports the repository's existing ctypes JNI helper. It runs original
methods with a null server and no players; it does not claim a normal server boot.
World.rand is explicitly seeded to 4242 for reproducibility. Classes are unmodified.

After x-major/z-minor loads, original World.setBlock installs glowstone (89),
torch (50), and stationary lava (11) at (8,75,8), (9,75,8), and (10,75,8), then
World.func_6156_d drains lighting and World.saveWorld(true,null) saves to McRegion.
Border chunks retain Beta's actual TerrainPopulated flags; they are not rewritten.
In the Nether the original plain setter also places then removes glowstone at
(8,127,8), replacing one roof block. Its height update leaves one valid saved
SkyLight nibble of 15 at that position. CaveWii's blanket Nether normalization
currently wipes it on disk load. This is an intentional original-JAR witness.

`stored-light.bin` independently decodes each saved NBT record into SkyLight
(16384), BlockLight (16384), HeightMap (256), in dimension 0,-1 then x,z order.
`east-light.bin` captures the original provider loading x=4,z=0..3, followed by
a full light drain; it contains those three arrays for x=0..4,z=0..3 per dimension.
Before that request, fresh original-JAR worlds reload all 32 saved chunks and
assert that their three arrays equal the independent NBT decode byte for byte.
The C test reads this frozen JNI oracle without requiring a Java installation.

The regression copies saves into a PID-isolated writable directory and exercises
the production level.dat and chunk loaders, eight unanchored server block-service ticks + drains,
and original local chunk-slot copy/stamping plus actual CRPC_CHUNK handlers for
both split world payloads. RPC assembly is fixture-driven, not a full Join or
network multiplayer session. It compares every client cell's stored light.

Seam diagnostics compare both imported/new and pure CaveWii boundaries to the
same original-JAR east-neighbour arrays. CaveWii's bare load method generates
terrain but defers population; the original provider populates immediately.
Thus the baseline bound is a diagnostic and does not establish exact seam parity.
The test retains nonzero exit status for any stored-array/client mismatch or a
maximum imported seam delta or differing-cell count greater than that baseline.
No known-fail suppression.

Beta citations (pinned JAR CFR decompile): ChunkLoader.java:163-185 restores all
valid saved arrays, McRegionChunkLoader.java:34-61 loads that NBT; Chunk.java:242-276
plain setBlockID updates height/Sky without the Nether guard present in its
metadata setter; World.java:1300 suppresses Sky queue admission only.
