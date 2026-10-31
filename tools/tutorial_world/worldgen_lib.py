#!/usr/bin/env python3
"""
worldgen_lib -- build an ORIGINAL, hand-authored Beta 1.7.3 / CaveWii save
(level.dat + McRegion .mcr files) from Python. Standard library only.

    from worldgen_lib import World
    w = World("My Tutorial", seed=0, spawn=(8, 65, 8))
    w.fill(0, 60, 0, 15, 64, 15, "grass")      # names or numeric ids
    w.torch(8, 65, 8)                            # standing torch
    w.save("saves/My Tutorial")                  # lights, packs, writes

Formats, and the CaveWii loader code that consumes each one:

  level.dat   gzip NBT, same fields/types as tools/gen_level_template.py
              (source/network/level_archive.c, level_archive.h tag table).
              CaveWiiSaveVersion = 5 (CAVEWII_SAVE_VERSION_CURRENT) so no
              legacy migration runs on load (server_local.c ~18676-18780).
  region      McRegion: 4 KiB offset table (offset<<8 | sectors), 4 KiB
              timestamp table (bit 31 = CaveWii "canonical record" flag, kept
              CLEAR so the first in-game save reads the record back and merges),
              records = u32 length, u8 type 2 (zlib), payload, 4 KiB sectors
              (region_archive.c region_archive_create_impl / region_read_record).
  chunk       .Level{Blocks 32768, Data/SkyLight/BlockLight 16384, HeightMap
              256 -- exact sizes or the chunk is rejected
              (region_archive_get_chunk_impl), Entities (Beta mob rows, see
              mob()), TileEntities, CaveWiiTileEntitiesComplete=1 (omitted on
              chunks with mobs, see chunk_nbt), LastUpdate, xPos, zPos,
              TerrainPopulated=1, CaveWiiWallSignMetadataVersion=2}.
              Tag order and types mirror the game's own writer
              (region_archive.c level_list_nodes) and every tile entity mirrors
              tile_entities_build() byte for byte, so a later in-game save of a
              chunk (or the player breaking a chest/sign) takes the game's
              normal "deletion safe" path (tile_entities_nbt.c
              te_native_deletion_safe compares dumps byte-for-byte).

Index conventions (Beta Chunk / NibbleArray, same in server_world.h):
  Blocks[x<<11 | z<<7 | y]; nibble i -> byte i>>1, low nibble when i even;
  HeightMap[z<<4 | x] = 1 + highest y whose light opacity != 0.

Light is PRECOMPUTED here because the loader stores it as-is (no relight on
load). The result is the Beta light engine's converged state:
  sky   = 15 for y >= HeightMap, else max(neighbour) - max(1, opacity)
  block = max(emission, max(neighbour) - max(1, opacity))
using the game's own emission/opacity table (light_tables.py, drift-checked
against source/block/*.c by tests/test_tutorial_worldgen_lib.py). Light does
not flow into chunks the world does not contain.
"""
import gzip
import os
import struct
import zlib
from collections import deque

try:  # package or script use
    from . import light_tables
except ImportError:  # pragma: no cover
    import light_tables

WORLD_HEIGHT = 128
CHUNK_BLOCKS = 16 * 16 * WORLD_HEIGHT
SECTOR = 4096
SAVE_VERSION = 5          # CAVEWII_SAVE_VERSION_CURRENT (level_archive.h)
WALL_SIGN_META_VERSION = 2  # region_archive.c WALL_SIGN_METADATA_VERSION
PLAYER_EYE_HEIGHT = 1.62  # Pos[1] is the eye (screen_select_world.c:804)
FULL_HEALTH = 160         # 10 hearts * 16 (gen_level_template.py)

# ---------------------------------------------------------------- block ids --
# Beta 1.7.3 ids (source/block/blocks_data.h enum block_type).
BLOCK_IDS = {
    "air": 0, "stone": 1, "grass": 2, "dirt": 3, "cobblestone": 4,
    "planks": 5, "sapling": 6, "bedrock": 7, "water_flowing": 8, "water": 9,
    "lava_flowing": 10, "lava": 11, "sand": 12, "gravel": 13, "gold_ore": 14,
    "iron_ore": 15, "coal_ore": 16, "log": 17, "leaves": 18, "sponge": 19,
    "glass": 20, "lapis_ore": 21, "lapis_block": 22, "dispenser": 23,
    "sandstone": 24, "noteblock": 25, "bed": 26, "powered_rail": 27,
    "detector_rail": 28, "sticky_piston": 29, "cobweb": 30, "tall_grass": 31,
    "dead_bush": 32, "piston": 33, "wool": 35, "dandelion": 37, "rose": 38,
    "brown_mushroom": 39, "red_mushroom": 40, "gold_block": 41,
    "iron_block": 42, "double_slab": 43, "slab": 44, "bricks": 45, "tnt": 46,
    "bookshelf": 47, "mossy_cobblestone": 48, "obsidian": 49, "torch": 50,
    "fire": 51, "spawner": 52, "wooden_stairs": 53, "chest": 54,
    "redstone_wire": 55, "diamond_ore": 56, "diamond_block": 57,
    "workbench": 58, "crops": 59, "farmland": 60, "furnace": 61,
    "furnace_lit": 62, "sign_post": 63, "wooden_door": 64, "ladder": 65,
    "rail": 66, "stone_stairs": 67, "wall_sign": 68, "lever": 69,
    "stone_pressure_plate": 70, "iron_door": 71, "wooden_pressure_plate": 72,
    "redstone_ore": 73, "redstone_torch_off": 75, "redstone_torch": 76,
    "stone_button": 77, "snow": 78, "ice": 79, "snow_block": 80,
    "cactus": 81, "clay": 82, "reed": 83, "jukebox": 84, "fence": 85,
    "pumpkin": 86, "netherrack": 87, "soul_sand": 88, "glowstone": 89,
    "jack_o_lantern": 91, "cake": 92, "repeater": 93, "repeater_lit": 94,
    "trapdoor": 96,
}
# Crafted-only ids that Beta never generates (tests/test_no_natural_bricks.c)
# are fine in an authored world; they are listed so tooling can flag them.
CRAFTED_ONLY = {5, 45, 47, 61, 62}

# Directions: +x east, -x west, +z south, -z north (Beta convention).
DIRS = {"east": (1, 0), "west": (-1, 0), "south": (0, 1), "north": (0, -1)}

# Facing -> metadata tables. Each cites the code that DECODES it in-game.
# Torch/redstone torch/button: 1..4 = mount at x-1, x+1, z-1, z+1 (block
# faces east/west/south/north), 5 = floor (block_torch.c:148-151,
# block_button.c:96-).
TORCH_META = {"east": 1, "west": 2, "south": 3, "north": 4, "floor": 5}
BUTTON_META = {"east": 1, "west": 2, "south": 3, "north": 4}
# Lever: same wall layout, 5/6 floor, bit 8 = on (block_lever.c:146-148,192-194).
LEVER_META = {"east": 1, "west": 2, "south": 3, "north": 4,
              "floor_ns": 5, "floor_ew": 6}
# Ladder / wall sign: 2..5 = faces north/south/west/east, i.e. mounted on the
# block at z+1 / z-1 / x+1 / x-1 (block_ladder.c:34-77, block_sign.c:187-188).
WALL_META = {"north": 2, "south": 3, "west": 4, "east": 5}
# Furnace / dispenser front face (Beta BlockFurnace onBlockPlacedBy).
FRONT_META = {"north": 2, "south": 3, "west": 4, "east": 5}
# Stairs: the HIGH side (block_stairs.c:148-156 onItemPlace: player on +x
# side -> 1, i.e. meta 0 rises toward +x).
STAIRS_META = {"east": 0, "west": 1, "south": 2, "north": 3}
# Repeater facing = OUTPUT side; facing 0/1/2/3 reads its input from
# +Z/-X/-Z/+X (redstone_repeater.h:33-35). Delay index in bits 2-3.
REPEATER_META = {"north": 0, "east": 1, "south": 2, "west": 3}
# Door: direction the placing player looked (into the doorway); closed panel
# state 0..3 = west/north/east/south edge (block_door.c:54-74 getBoundingBox;
# Beta ItemDoor yaw formula). Bit 4 open, bit 8 upper half.
DOOR_META = {"east": 0, "south": 1, "west": 2, "north": 3}
# Standing sign: text faces south at 0, west 4, north 8, east 12 (16 steps).
SIGN_POST_META = {"south": 0, "west": 4, "north": 8, "east": 12}
# Bed: Beta 1.7.3's own layout (PAR-BED-META, source/block/bed_meta.h;
# BlockBed.headBlockToFootBlockMap / ItemBed var9): foot = dir, head = dir|8,
# dir 0 south (+Z), 1 west (-X), 2 north (-Z), 3 east (+X); 0x4 = occupied
# (never written here). Save version 5 means the loader runs no bed
# conversion on these worlds.
BED_DIR = {"south": 0, "west": 1, "north": 2, "east": 3}
# Rails: 0 N-S, 1 E-W, 2-5 ascending east/west/north/south, 6-9 curves
# (south-east, south-west, north-west, north-east).
RAIL_META = {"ns": 0, "ew": 1, "up_east": 2, "up_west": 3, "up_north": 4,
             "up_south": 5, "se": 6, "sw": 7, "nw": 8, "ne": 9}
SLAB_META = {"stone": 0, "sandstone": 1, "wood": 2, "cobblestone": 3}
# Sapling / log / leaves wood type (Beta: 0 oak, 1 spruce, 2 birch). Beta 1.7.3
# logs have NO axis. Leaves bits 2-3 are decay state: leave them 0 and keep a
# log within 4 blocks or the leaves decay.
WOOD_META = {"oak": 0, "spruce": 1, "birch": 2}
SAPLING_META = WOOD_META
# Wool colours (Beta ItemDye / BlockCloth damage values).
WOOL_META = {"white": 0, "orange": 1, "magenta": 2, "light_blue": 3,
             "yellow": 4, "lime": 5, "pink": 6, "gray": 7, "light_gray": 8,
             "cyan": 9, "purple": 10, "blue": 11, "brown": 12, "green": 13,
             "red": 14, "black": 15}
# Piston facing (Beta Facing: 0 down, 1 up, 2 -z, 3 +z, 4 -x, 5 +x), bit 8 =
# extended; the head (34) carries the facing plus bit 8 when sticky
# (redstone_piston.h piston_orientation / piston_is_powered).
PISTON_META = {"down": 0, "up": 1, "north": 2, "south": 3, "west": 4, "east": 5}
PISTON_OFFSET = {0: (0, -1, 0), 1: (0, 1, 0), 2: (0, 0, -1), 3: (0, 0, 1),
                 4: (-1, 0, 0), 5: (1, 0, 0)}
# Pumpkin / jack-o'-lantern face direction (block_pumpkin.c onItemPlace:
# player at +z -> 0 ... the face looks back at the player).
PUMPKIN_META = {"south": 0, "west": 1, "north": 2, "east": 3}
# Trapdoor: low 2 bits = the side its hinge/support block is on: z+1, z-1,
# x+1, x-1 (block_trapdoor.c:57-74); bit 4 open.
TRAPDOOR_META = {"south": 0, "north": 1, "east": 2, "west": 3}
TALL_GRASS_META = {"dead_shrub": 0, "grass": 1, "fern": 2}


def block_id(b):
    if isinstance(b, int):
        if not 0 <= b <= 255:
            raise ValueError("block id out of range: %r" % b)
        return b
    try:
        return BLOCK_IDS[b]
    except KeyError:
        raise ValueError("unknown block name %r" % b) from None


def _dir(facing, table, what):
    try:
        return table[facing]
    except KeyError:
        raise ValueError("%s: facing must be one of %s, got %r"
                         % (what, sorted(table), facing)) from None


# --------------------------------------------------------------------- NBT --
TAG_END, TAG_BYTE, TAG_SHORT, TAG_INT, TAG_LONG, TAG_FLOAT, TAG_DOUBLE, \
    TAG_BYTE_ARRAY, TAG_STRING, TAG_LIST, TAG_COMPOUND = range(11)


class Tag:
    """One typed NBT value. Compound values are lists of (name, Tag) pairs so
    the field ORDER is explicit (the game compares dumps byte for byte)."""
    __slots__ = ("type", "value", "elem")

    def __init__(self, tid, value, elem=None):
        self.type, self.value, self.elem = tid, value, elem

    def __eq__(self, other):
        return (isinstance(other, Tag) and self.type == other.type
                and self.value == other.value and self.elem == other.elem)

    def __repr__(self):
        return "Tag(%d, %r)" % (self.type, self.value)


def Byte(v): return Tag(TAG_BYTE, int(v))
def Short(v): return Tag(TAG_SHORT, int(v))
def Int(v): return Tag(TAG_INT, int(v))
def Long(v): return Tag(TAG_LONG, int(v))
def Float(v): return Tag(TAG_FLOAT, float(v))
def Double(v): return Tag(TAG_DOUBLE, float(v))
def ByteArray(b): return Tag(TAG_BYTE_ARRAY, bytes(b))
def String(s): return Tag(TAG_STRING, str(s))
def Compound(pairs): return Tag(TAG_COMPOUND, list(pairs))


def List(elem_type, items):
    """A TAG_List. An EMPTY list keeps the element type you give it; Beta's
    own writer uses TAG_BYTE (1) for empty item lists and the game's chunk
    writer TAG_COMPOUND (10) for empty Entities/TileEntities."""
    items = list(items)
    for it in items:
        if it.type != elem_type:
            raise ValueError("list element type mismatch")
    return Tag(TAG_LIST, items, elem_type)


def _enc_string(s):
    raw = s.encode("utf-8")
    if any(c == 0 for c in raw):
        raise ValueError("NUL in NBT string")
    if len(raw) > 0xFFFF:
        raise ValueError("NBT string too long")
    return struct.pack(">H", len(raw)) + raw


def encode_payload(tag):
    t, v = tag.type, tag.value
    if t == TAG_BYTE:
        return struct.pack(">b", v)
    if t == TAG_SHORT:
        return struct.pack(">h", v)
    if t == TAG_INT:
        return struct.pack(">i", v)
    if t == TAG_LONG:
        return struct.pack(">q", v)
    if t == TAG_FLOAT:
        return struct.pack(">f", v)
    if t == TAG_DOUBLE:
        return struct.pack(">d", v)
    if t == TAG_BYTE_ARRAY:
        return struct.pack(">i", len(v)) + v
    if t == TAG_STRING:
        return _enc_string(v)
    if t == TAG_LIST:
        return (bytes([tag.elem]) + struct.pack(">i", len(v))
                + b"".join(encode_payload(x) for x in v))
    if t == TAG_COMPOUND:
        return b"".join(encode_named(n, x) for n, x in v) + b"\x00"
    raise ValueError("bad tag type %r" % t)


def encode_named(name, tag):
    return bytes([tag.type]) + _enc_string(name) + encode_payload(tag)


def encode_root(tag, name=""):
    if tag.type != TAG_COMPOUND:
        raise ValueError("root must be a compound")
    return encode_named(name, tag)


def decode_root(data):
    """Parse an uncompressed NBT document -> (root_name, Tag)."""
    pos = [0]

    def take(n):
        if pos[0] + n > len(data):
            raise ValueError("truncated NBT")
        b = data[pos[0]:pos[0] + n]
        pos[0] += n
        return b

    def string():
        (n,) = struct.unpack(">H", take(2))
        return take(n).decode("utf-8")

    def payload(t):
        if t == TAG_BYTE:
            return Byte(struct.unpack(">b", take(1))[0])
        if t == TAG_SHORT:
            return Short(struct.unpack(">h", take(2))[0])
        if t == TAG_INT:
            return Int(struct.unpack(">i", take(4))[0])
        if t == TAG_LONG:
            return Long(struct.unpack(">q", take(8))[0])
        if t == TAG_FLOAT:
            return Float(struct.unpack(">f", take(4))[0])
        if t == TAG_DOUBLE:
            return Double(struct.unpack(">d", take(8))[0])
        if t == TAG_BYTE_ARRAY:
            (n,) = struct.unpack(">i", take(4))
            return ByteArray(take(n))
        if t == TAG_STRING:
            return String(string())
        if t == TAG_LIST:
            et = take(1)[0]
            (n,) = struct.unpack(">i", take(4))
            return Tag(TAG_LIST, [payload(et) for _ in range(n)], et)
        if t == TAG_COMPOUND:
            pairs = []
            while True:
                ct = take(1)[0]
                if ct == TAG_END:
                    return Compound(pairs)
                nm = string()
                pairs.append((nm, payload(ct)))
        raise ValueError("bad tag type %d" % t)

    t = take(1)[0]
    if t != TAG_COMPOUND:
        raise ValueError("root is not a compound")
    name = string()
    root = payload(TAG_COMPOUND)
    if pos[0] != len(data):
        raise ValueError("trailing bytes after NBT root")
    return name, root


def child(compound, name):
    for n, v in compound.value:
        if n == name:
            return v
    return None


def path(tag, dotted):
    for part in dotted.strip(".").split("."):
        tag = child(tag, part)
        if tag is None:
            return None
    return tag


# ---------------------------------------------------------- tile entities --
def item_tag(slot, item_id, count=1, damage=0):
    """Item compound in the game's canonical field order
    (item_nbt.c canonical(): Slot, id, Count, Damage)."""
    if not (0 < item_id <= 32767 and 0 < count <= 127 and 0 <= damage <= 32767):
        raise ValueError("bad item (id=%r count=%r damage=%r)"
                         % (item_id, count, damage))
    pairs = []
    if slot is not None:
        pairs.append(("Slot", Byte(slot)))
    pairs += [("id", Short(item_id)), ("Count", Byte(count)),
              ("Damage", Short(damage))]
    return Compound(pairs)


def _items_list(items, n_slots):
    """items: iterable of (slot, id, count[, damage]). Sorted by slot, as
    tile_entities_build emits them; empty list uses element type TAG_BYTE."""
    seen = {}
    for it in items:
        slot, iid, cnt = it[0], it[1], it[2]
        dmg = it[3] if len(it) > 3 else 0
        if not 0 <= slot < n_slots:
            raise ValueError("slot %r outside 0..%d" % (slot, n_slots - 1))
        if slot in seen:
            raise ValueError("duplicate slot %r" % slot)
        seen[slot] = item_tag(slot, iid, cnt, dmg)
    if not seen:
        return List(TAG_BYTE, [])
    return List(TAG_COMPOUND, [seen[s] for s in sorted(seen)])


def _te_head(kind, x, y, z):
    return [("id", String(kind)), ("x", Int(x)), ("y", Int(y)), ("z", Int(z))]


SIGN_LINE_MAX = 15  # TE_SIGN_LINE_MAX, Beta TileEntitySign.readFromNBT clamp


def sign_te(x, y, z, lines):
    lines = list(lines)
    if len(lines) > 4:
        raise ValueError("a sign has at most 4 lines")
    lines += [""] * (4 - len(lines))
    for ln in lines:
        if len(ln) > SIGN_LINE_MAX:
            raise ValueError("sign line %r is longer than %d characters"
                             % (ln, SIGN_LINE_MAX))
        if any(not (32 <= ord(c) < 127) for c in ln):
            raise ValueError("sign line %r: printable ASCII only" % ln)
        if ln != ln.rstrip(" "):
            # tile_entities_build trims trailing spaces; a stored trailing
            # space would make the record differ from the game's own dump.
            raise ValueError("sign line %r has trailing spaces" % ln)
    return Compound(_te_head("Sign", x, y, z)
                    + [("Text%d" % (i + 1), String(ln))
                       for i, ln in enumerate(lines)])


def chest_te(x, y, z, items):
    return Compound(_te_head("Chest", x, y, z)
                    + [("Items", _items_list(items, 27))])


def furnace_te(x, y, z, items=(), burn_time=0, cook_time=0):
    return Compound(_te_head("Furnace", x, y, z)
                    + [("BurnTime", Short(burn_time)),
                       ("CookTime", Short(cook_time)),
                       ("Items", _items_list(items, 3))])


def dispenser_te(x, y, z, items=()):
    return Compound(_te_head("Trap", x, y, z)
                    + [("Items", _items_list(items, 9))])


def note_te(x, y, z, note=0):
    if not 0 <= note <= 24:
        raise ValueError("note 0..24")
    return Compound(_te_head("Music", x, y, z) + [("note", Byte(note))])


# ---------------------------------------------------------------- entities --
# Beta 1.7.3 passive mobs, written EXACTLY as Beta writes them so CaveWii's
# strict import check (source/network/entity_archive.c entity_beta_import_owned)
# owns every row. Citations are ../mcb173-retromcp/minecraft/src/net/minecraft/
# src/*.java (the RetroMCP decompile):
#   Entity.addEntityID:739-742    setString("id", getEntityString()), then
#   Entity.writeToNBT:750-758     Pos (3 doubles, posY + ySize; ySize = 0 at
#                                 rest), Motion (3 doubles), Rotation (2 floats
#                                 yaw, pitch), FallDistance (float), Fire
#                                 (short), Air (short), OnGround (boolean ->
#                                 byte), then writeEntityToNBT:
#   EntityLiving:557-562          Health, HurtTime, DeathTime, AttackTime (shorts)
#   EntitySheep:55-59             Sheared (boolean -> byte), Color (byte)
#   EntityPig:14-17               Saddle (boolean -> byte)
#   EntityCow:10-12, EntityChicken:54-56   nothing extra
#   EntityList:97-100             ids "Pig", "Sheep", "Cow", "Chicken"
# Values for a mob standing still on the ground: Fire = -fireResistance = -1
# (Entity.java:52 fireResistance = 1, :489-490), Air = maxAir = 300
# (Entity.java:54,57), Health = 10 (EntityLiving.java:26) except the chicken's
# 4 (EntityChicken.java:16). Beta's NBTTagCompound is a HashMap (no on-disk
# order); this writer uses the call order above, and the CaveWii check is
# order-free but rejects any missing, duplicate, extra or wrong-type tag.
MOB_HEALTH = {"Sheep": 10, "Pig": 10, "Cow": 10, "Chicken": 4}
MOB_HEIGHT = {"Sheep": 1.3, "Pig": 0.9, "Cow": 1.3, "Chicken": 0.4}  # setSize
WOOL_COLOURS = ("white", "orange", "magenta", "light_blue", "yellow", "lime",
                "pink", "gray", "light_gray", "cyan", "purple", "blue",
                "brown", "green", "red", "black")


def mob(x, y, z, kind, yaw=0.0, color=0, sheared=False, saddle=False):
    """One Beta 1.7.3 passive-mob Entities row. (x, y, z) is the FEET position
    (doubles; y is the top of the block it stands on). kind is "Sheep", "Cow",
    "Pig" or "Chicken". color: 0..15 or a WOOL_COLOURS name (sheep only);
    sheared (sheep only); saddle (pig only)."""
    if kind not in MOB_HEALTH:
        raise ValueError("unsupported mob kind %r" % (kind,))
    if isinstance(color, str):
        color = WOOL_COLOURS.index(color)
    if kind != "Sheep" and (color or sheared):
        raise ValueError("color/sheared are sheep tags")
    if kind != "Pig" and saddle:
        raise ValueError("saddle is a pig tag")
    if not 0 <= int(color) <= 15:
        raise ValueError("wool colour 0..15")
    pairs = [
        ("id", String(kind)),
        ("Pos", List(TAG_DOUBLE, [Double(x), Double(y), Double(z)])),
        ("Motion", List(TAG_DOUBLE, [Double(0), Double(0), Double(0)])),
        ("Rotation", List(TAG_FLOAT, [Float(yaw), Float(0)])),
        ("FallDistance", Float(0)),
        ("Fire", Short(-1)),
        ("Air", Short(300)),
        ("OnGround", Byte(1)),
        ("Health", Short(MOB_HEALTH[kind])),
        ("HurtTime", Short(0)),
        ("DeathTime", Short(0)),
        ("AttackTime", Short(0)),
    ]
    if kind == "Sheep":
        pairs += [("Sheared", Byte(1 if sheared else 0)),
                  ("Color", Byte(int(color)))]
    elif kind == "Pig":
        pairs.append(("Saddle", Byte(1 if saddle else 0)))
    return Compound(pairs)


# ------------------------------------------------------------------ chunks --
class Chunk:
    __slots__ = ("cx", "cz", "blocks", "meta", "tile_entities", "entities",
                 "sky", "blocklight", "heightmap")

    def __init__(self, cx, cz):
        self.cx, self.cz = cx, cz
        self.blocks = bytearray(CHUNK_BLOCKS)
        self.meta = bytearray(CHUNK_BLOCKS)  # one nibble per byte; packed on save
        self.tile_entities = {}              # (x, y, z) world -> Tag
        self.entities = []                   # Entities rows, in add order
        self.sky = None
        self.blocklight = None
        self.heightmap = None


def idx(lx, y, lz):
    return (lx << 11) | (lz << 7) | y


def pack_nibbles(values):
    out = bytearray(len(values) // 2)
    for i in range(0, len(values), 2):
        out[i >> 1] = (values[i] & 15) | ((values[i + 1] & 15) << 4)
    return bytes(out)


def unpack_nibbles(data):
    out = bytearray(len(data) * 2)
    for i, b in enumerate(data):
        out[2 * i] = b & 15
        out[2 * i + 1] = b >> 4
    return out


# ------------------------------------------------------------------- world --
class World:
    def __init__(self, level_name, seed=0, spawn=(0, 64, 0), time=1000,
                 player_pos=None, player_yaw=0.0, inventory=(),
                 timestamp=1, generator=1, raining=False, rain_time=0):
        if len(level_name.encode("utf-8")) > 64:
            raise ValueError("LevelName too long")
        self.level_name = level_name
        self.seed = seed
        self.spawn = tuple(int(v) for v in spawn)
        self.time = time
        # Feet position; defaults to the centre of the spawn block.
        self.player_pos = (player_pos if player_pos is not None else
                           (self.spawn[0] + 0.5, float(self.spawn[1]),
                            self.spawn[2] + 0.5))
        self.player_yaw = player_yaw
        self.inventory = list(inventory)  # (slot, id, count[, damage])
        self.timestamp = timestamp & 0x7FFFFFFF  # bit 31 CLEAR (canon flag)
        self.generator = generator
        self.raining = raining
        self.rain_time = rain_time
        self.chunks = {}

    # -- raw access -------------------------------------------------------
    def chunk(self, cx, cz, create=True):
        c = self.chunks.get((cx, cz))
        if c is None and create:
            c = self.chunks[(cx, cz)] = Chunk(cx, cz)
        return c

    def ensure_area(self, x1, z1, x2, z2):
        """Make every chunk covering the block rectangle exist (air)."""
        for cx in range(min(x1, x2) >> 4, (max(x1, x2) >> 4) + 1):
            for cz in range(min(z1, z2) >> 4, (max(z1, z2) >> 4) + 1):
                self.chunk(cx, cz)

    def set(self, x, y, z, block, meta=0):
        if not 0 <= y < WORLD_HEIGHT:
            raise ValueError("y=%d outside 0..%d" % (y, WORLD_HEIGHT - 1))
        if not 0 <= meta <= 15:
            raise ValueError("metadata %r outside 0..15" % meta)
        c = self.chunk(x >> 4, z >> 4)
        i = idx(x & 15, y, z & 15)
        c.blocks[i] = block_id(block)
        c.meta[i] = meta
        c.tile_entities.pop((x, y, z), None)

    def get(self, x, y, z):
        c = self.chunks.get((x >> 4, z >> 4))
        if c is None or not 0 <= y < WORLD_HEIGHT:
            return (0, 0)
        i = idx(x & 15, y, z & 15)
        return (c.blocks[i], c.meta[i])

    def set_tile_entity(self, x, y, z, tag):
        self.chunk(x >> 4, z >> 4).tile_entities[(x, y, z)] = tag

    def mob(self, x, y, z, kind, **opts):
        """Place a passive mob (see the module-level mob()) at feet (x, y, z)
        in the chunk that owns that position."""
        tag = mob(x, y, z, kind, **opts)
        self.chunk(int(x // 1) >> 4, int(z // 1) >> 4).entities.append(tag)
        return tag

    def entities(self):
        """Every Entities row, chunk by chunk."""
        return [e for _, c in sorted(self.chunks.items()) for e in c.entities]

    def tile_entity(self, x, y, z):
        c = self.chunks.get((x >> 4, z >> 4))
        return c.tile_entities.get((x, y, z)) if c else None

    # -- shapes -----------------------------------------------------------
    def fill(self, x1, y1, z1, x2, y2, z2, block, meta=0):
        for x in range(min(x1, x2), max(x1, x2) + 1):
            for z in range(min(z1, z2), max(z1, z2) + 1):
                for y in range(min(y1, y2), max(y1, y2) + 1):
                    self.set(x, y, z, block, meta)

    box = fill

    def hollow_box(self, x1, y1, z1, x2, y2, z2, block, meta=0, inside="air"):
        xa, xb = sorted((x1, x2))
        ya, yb = sorted((y1, y2))
        za, zb = sorted((z1, z2))
        for x in range(xa, xb + 1):
            for z in range(za, zb + 1):
                for y in range(ya, yb + 1):
                    edge = x in (xa, xb) or y in (ya, yb) or z in (za, zb)
                    if edge:
                        self.set(x, y, z, block, meta)
                    elif inside is not None:
                        self.set(x, y, z, inside)

    def line(self, x1, y1, z1, x2, y2, z2, block, meta=0):
        """Straight voxel line (each axis stepped proportionally)."""
        n = max(abs(x2 - x1), abs(y2 - y1), abs(z2 - z1))
        for i in range(n + 1):
            t = i / n if n else 0.0
            self.set(round(x1 + (x2 - x1) * t), round(y1 + (y2 - y1) * t),
                     round(z1 + (z2 - z1) * t), block, meta)

    # -- placements with metadata ---------------------------------------
    def stairs(self, x, y, z, ascends, material="wood"):
        bid = {"wood": 53, "stone": 67, "cobblestone": 67}[material]
        self.set(x, y, z, bid, _dir(ascends, STAIRS_META, "stairs"))

    def slab(self, x, y, z, kind="stone", double=False):
        self.set(x, y, z, 43 if double else 44, _dir(kind, SLAB_META, "slab"))

    def door(self, x, y, z, facing, hinge="left", material="wood",
             open_=False):
        """Two-block door; (x,y,z) is the lower half. `facing` = the way the
        placing player looked. hinge='right' is Beta ItemDoor's mirrored
        placement ((m-1)&3 with the open bit set) -- it LOOKS closed."""
        bid = {"wood": 64, "iron": 71}[material]
        m = _dir(facing, DOOR_META, "door")
        if hinge == "right":
            m = ((m - 1) & 3) | 4
        elif hinge != "left":
            raise ValueError("hinge must be 'left' or 'right'")
        if open_:
            m ^= 4
        self.set(x, y, z, bid, m)
        self.set(x, y + 1, z, bid, m | 8)

    def torch(self, x, y, z, facing="floor", redstone=False, lit=True):
        bid = 50 if not redstone else (76 if lit else 75)
        self.set(x, y, z, bid, _dir(facing, TORCH_META, "torch"))

    def lever(self, x, y, z, facing, on=False):
        self.set(x, y, z, 69, _dir(facing, LEVER_META, "lever") | (8 if on else 0))

    def button(self, x, y, z, facing):
        self.set(x, y, z, 77, _dir(facing, BUTTON_META, "button"))

    def ladder(self, x, y, z, facing):
        self.set(x, y, z, 65, _dir(facing, WALL_META, "ladder"))

    def sign(self, x, y, z, lines, facing="south", wall=False):
        """wall=True: wall sign facing north/south/west/east (mounted on the
        opposite neighbour). wall=False: sign post; facing is a compass name
        or a 0..15 rotation."""
        te = sign_te(x, y, z, lines)  # validate before touching the world
        if wall:
            self.set(x, y, z, 68, _dir(facing, WALL_META, "wall sign"))
        else:
            rot = facing if isinstance(facing, int) else _dir(
                facing, SIGN_POST_META, "sign post")
            self.set(x, y, z, 63, rot)
        self.set_tile_entity(x, y, z, te)

    def chest(self, x, y, z, items=()):
        te = chest_te(x, y, z, items)
        self.set(x, y, z, 54, 0)
        self.set_tile_entity(x, y, z, te)

    def furnace(self, x, y, z, facing="south", lit=False, items=(),
                burn_time=None, cook_time=0):
        if burn_time is None:
            burn_time = 1600 if lit else 0
        if lit and burn_time <= 0:
            raise ValueError("a lit furnace needs burn_time > 0")
        te = furnace_te(x, y, z, items, burn_time, cook_time)
        self.set(x, y, z, 62 if lit else 61, _dir(facing, FRONT_META, "furnace"))
        self.set_tile_entity(x, y, z, te)

    def dispenser(self, x, y, z, facing="south", items=()):
        te = dispenser_te(x, y, z, items)
        self.set(x, y, z, 23, _dir(facing, FRONT_META, "dispenser"))
        self.set_tile_entity(x, y, z, te)

    def noteblock(self, x, y, z, note=0):
        te = note_te(x, y, z, note)
        self.set(x, y, z, 25, 0)
        self.set_tile_entity(x, y, z, te)

    def repeater(self, x, y, z, facing, delay=1, powered=False):
        """facing = OUTPUT direction; delay 1..4 (x2 game ticks)."""
        if not 1 <= delay <= 4:
            raise ValueError("repeater delay is 1..4")
        m = _dir(facing, REPEATER_META, "repeater") | ((delay - 1) << 2)
        self.set(x, y, z, 94 if powered else 93, m)

    def wire(self, x, y, z, power=0):
        if not 0 <= power <= 15:
            raise ValueError("wire power 0..15")
        self.set(x, y, z, 55, power)

    def rail(self, x, y, z, shape="ns"):
        self.set(x, y, z, 66, _dir(shape, RAIL_META, "rail"))

    def powered_rail(self, x, y, z, shape="ns", powered=False, detector=False):
        """Powered (27) or detector (28) rail: straight/ascending shapes only
        (low 3 bits 0-5), bit 8 = powered (block_rail.c:54-75)."""
        m = _dir(shape, RAIL_META, "powered rail")
        if m > 5:
            raise ValueError("powered/detector rails cannot curve")
        self.set(x, y, z, 28 if detector else 27, m | (8 if powered else 0))

    def log(self, x, y, z, kind="oak"):
        self.set(x, y, z, 17, _dir(kind, WOOD_META, "log"))

    def leaves(self, x, y, z, kind="oak"):
        self.set(x, y, z, 18, _dir(kind, WOOD_META, "leaves"))

    def wool(self, x, y, z, colour="white"):
        self.set(x, y, z, 35, _dir(colour, WOOL_META, "wool"))

    def piston(self, x, y, z, facing, sticky=False, extended=False):
        """Piston base; extended=True also places the head (34) in front."""
        f = _dir(facing, PISTON_META, "piston")
        self.set(x, y, z, 29 if sticky else 33, f | (8 if extended else 0))
        if extended:
            dx, dy, dz = PISTON_OFFSET[f]
            self.set(x + dx, y + dy, z + dz, 34, f | (8 if sticky else 0))

    def pumpkin(self, x, y, z, facing="south", lit=False):
        self.set(x, y, z, 91 if lit else 86, _dir(facing, PUMPKIN_META, "pumpkin"))

    def trapdoor(self, x, y, z, hinge_side, open_=False):
        self.set(x, y, z, 96, _dir(hinge_side, TRAPDOOR_META, "trapdoor")
                 | (4 if open_ else 0))

    def tall_grass(self, x, y, z, kind="grass"):
        self.set(x, y, z, 31, _dir(kind, TALL_GRASS_META, "tall grass"))

    def reed(self, x, y, z, height=1):
        """Sugar cane column; needs sand/grass/dirt below and water next to
        that block (Beta BlockReed.canPlaceBlockAt)."""
        for i in range(height):
            self.set(x, y + i, z, 83, 0)

    def cactus(self, x, y, z, height=1):
        for i in range(height):
            self.set(x, y + i, z, 81, 0)

    def pressure_plate(self, x, y, z, material="stone"):
        self.set(x, y, z, {"stone": 70, "wood": 72}[material], 0)

    def jukebox(self, x, y, z):
        self.set(x, y, z, 84, 0)  # no record: Beta writes no TE row needed

    def dark_spawn_spots(self, x1, z1, x2, z2):
        """Cells a Beta monster could spawn on at night: air with air above
        and an opaque block below whose BLOCK light is <= 7 (night sky light
        is 15-11 = 4, so block light decides; Beta SpawnerAnimals /
        EntityMob.getCanSpawnHere light <= rand(8)). Run after
        compute_light(). Returns [(x, y, z)] of the standing cell."""
        out = []
        for x in range(min(x1, x2), max(x1, x2) + 1):
            for z in range(min(z1, z2), max(z1, z2) + 1):
                c = self.chunks.get((x >> 4, z >> 4))
                if c is None:
                    continue
                for y in range(1, WORLD_HEIGHT - 1):
                    i = idx(x & 15, y, z & 15)
                    if (c.blocks[i] == 0 and c.blocks[i + 1] == 0
                            and light_tables.light_of(c.blocks[i - 1])[1] == 255
                            and c.blocklight[i] <= 7):
                        out.append((x, y, z))
        return out

    def bed(self, x, y, z, facing):
        """Foot at (x,y,z), head one block toward `facing` (Beta metadata,
        see BED_DIR)."""
        d = _dir(facing, BED_DIR, "bed")
        dx, dz = DIRS[facing]
        self.set(x, y, z, 26, d)
        self.set(x + dx, y, z + dz, 26, d | 8)

    def crop(self, x, y, z, stage=7, farmland_moisture=7):
        if not 0 <= stage <= 7:
            raise ValueError("crop stage 0..7")
        self.set(x, y - 1, z, 60, farmland_moisture)
        self.set(x, y, z, 59, stage)

    def sapling(self, x, y, z, kind="oak"):
        self.set(x, y, z, 6, _dir(kind, SAPLING_META, "sapling"))

    def flower(self, x, y, z, kind="dandelion"):
        self.set(x, y, z, {"dandelion": 37, "yellow": 37, "rose": 38}[kind])

    def water_source(self, x, y, z):
        self.set(x, y, z, 9, 0)

    def lava_source(self, x, y, z):
        self.set(x, y, z, 11, 0)

    # -- checks -------------------------------------------------------------
    def problems(self):
        """Cheap authoring checks: attachables without a mount, half doors,
        crops off farmland, tile-entity blocks without/with a stray TE."""
        out = []
        te_blocks = {54: "Chest", 63: "Sign", 68: "Sign", 61: "Furnace",
                     62: "Furnace", 23: "Trap", 25: "Music"}
        solid = lambda p: light_tables.light_of(self.get(*p)[0])[1] == 255
        for (cx, cz), c in sorted(self.chunks.items()):
            for i, b in enumerate(c.blocks):
                if not b:
                    continue
                lx, lz, y = i >> 11, (i >> 7) & 15, i & 127
                x, z = (cx << 4) | lx, (cz << 4) | lz
                m = c.meta[i]
                where = "%s@(%d,%d,%d)" % (b, x, y, z)
                if b in te_blocks:
                    te = c.tile_entities.get((x, y, z))
                    if te is None or child(te, "id").value != te_blocks[b]:
                        out.append("missing %s tile entity: %s"
                                   % (te_blocks[b], where))
                if b in (50, 75, 76, 77) or (b == 69 and (m & 7) <= 4):
                    mm = m & 7
                    off = {1: (-1, 0, 0), 2: (1, 0, 0), 3: (0, 0, -1),
                           4: (0, 0, 1), 5: (0, -1, 0)}.get(mm)
                    if off is None or (b == 77 and mm == 5):
                        out.append("bad attach metadata: " + where)
                    elif not solid((x + off[0], y + off[1], z + off[2])):
                        out.append("no mount block: " + where)
                if b in (65, 68):
                    off = {2: (0, 0, 1), 3: (0, 0, -1), 4: (1, 0, 0),
                           5: (-1, 0, 0)}.get(m)
                    if off is None or not solid((x + off[0], y, z + off[2])):
                        out.append("no mount block: " + where)
                if b in (55, 93, 94, 66, 70, 72) and not solid((x, y - 1, z)):
                    out.append("needs a solid block below: " + where)
                if b == 59 and self.get(x, y - 1, z)[0] != 60:
                    out.append("crop not on farmland: " + where)
                if b in (64, 71):
                    other = self.get(x, y + (-1 if m & 8 else 1), z)
                    if other[0] != b or (other[1] & 8) == (m & 8):
                        out.append("half door: " + where)
            for e in c.entities:
                ex, ey, ez = (v.value for v in child(e, "Pos").value)
                kind = child(e, "id").value
                bx, by, bz = int(ex // 1), int(ey // 1), int(ez // 1)
                where = "%s@(%.2f,%.2f,%.2f)" % (kind, ex, ey, ez)
                if (bx >> 4, bz >> 4) != (cx, cz):
                    out.append("entity outside its chunk: " + where)
                if ey != by or not solid((bx, by - 1, bz)):
                    out.append("mob not standing on a full block: " + where)
                top = int((ey + MOB_HEIGHT.get(kind, 1.8)) // 1)
                for yy in range(by, top + 1):
                    if solid((bx, yy, bz)) or self.get(bx, yy, bz)[0] in (
                            8, 9, 10, 11, 85):
                        out.append("mob inside a block: " + where)
                        break
            for (x, y, z), te in c.tile_entities.items():
                if c.blocks[idx(x & 15, y, z & 15)] not in te_blocks:
                    out.append("tile entity without its block at (%d,%d,%d)"
                               % (x, y, z))
        return out

    # -- light ------------------------------------------------------------
    def compute_light(self, table=light_tables.BAKED_LIGHT_TABLE):
        """Fill every chunk's heightmap, sky light and block light with the
        converged Beta values (see module docstring)."""
        if not self.chunks:
            return
        emit = bytearray(256)
        opac = bytearray(256)  # 1..15 attenuation step, 16 = blocks light
        hmop = bytearray(256)  # raw opacity != 0 -> counts for HeightMap
        for b in range(256):
            e, o = light_tables.light_of(b, table)
            emit[b] = e
            opac[b] = 16 if o >= 15 else max(1, o)
            hmop[b] = 1 if o else 0
        xs = [c[0] for c in self.chunks]
        zs = [c[1] for c in self.chunks]
        cx0, cz0 = min(xs), min(zs)
        W, D = (max(xs) - cx0 + 1) * 16, (max(zs) - cz0 + 1) * 16
        H = WORLD_HEIGHT
        n = W * D * H
        ids = bytearray(n)
        present = bytearray(W * D)  # column belongs to an existing chunk
        for (cx, cz), c in self.chunks.items():
            bx, bz = (cx - cx0) * 16, (cz - cz0) * 16
            for lx in range(16):
                for lz in range(16):
                    col = ((bx + lx) * D + (bz + lz))
                    present[col] = 1
                    src = (lx << 11) | (lz << 7)
                    ids[col * H:col * H + H] = c.blocks[src:src + H]
        heights = bytearray(W * D)
        for col in range(W * D):
            if not present[col]:
                continue
            base = col * H
            h = H
            while h > 0 and not hmop[ids[base + h - 1]]:
                h -= 1
            heights[col] = h

        def flood(light, queue):
            pop, push = queue.popleft, queue.append
            while queue:
                i = pop()
                v = light[i]
                if v <= 1:
                    continue
                col, y = divmod(i, H)
                x, z = divmod(col, D)
                for ncol, ny in ((col, y + 1), (col, y - 1),
                                 (col - D if x > 0 else -1, y),
                                 (col + D if x < W - 1 else -1, y),
                                 (col - 1 if z > 0 else -1, y),
                                 (col + 1 if z < D - 1 else -1, y)):
                    if ncol < 0 or not 0 <= ny < H or not present[ncol]:
                        continue
                    j = ncol * H + ny
                    nv = v - opac[ids[j]]
                    if nv > light[j]:
                        light[j] = nv
                        push(j)

        sky = bytearray(n)
        q = deque()
        for col in range(W * D):
            if not present[col]:
                continue
            base = col * H
            h = heights[col]
            for y in range(h, H):
                sky[base + y] = 15
            # Only cells that can hand light to a darker neighbour need
            # seeding: the cell at the height (lights downward) and any cell
            # below a neighbouring column's height (lights sideways).
            x, z = divmod(col, D)
            top = h
            for nc in ((col - D) if x > 0 else -1, (col + D) if x < W - 1 else -1,
                       (col - 1) if z > 0 else -1, (col + 1) if z < D - 1 else -1):
                if nc >= 0 and present[nc] and heights[nc] > top:
                    top = heights[nc]
            for y in range(h, min(top + 1, H)):
                q.append(base + y)
        flood(sky, q)

        blk = bytearray(n)
        q = deque()
        for i in range(n):
            e = emit[ids[i]]
            if e:
                blk[i] = e
                q.append(i)
        flood(blk, q)

        for (cx, cz), c in self.chunks.items():
            bx, bz = (cx - cx0) * 16, (cz - cz0) * 16
            s = bytearray(CHUNK_BLOCKS)
            b = bytearray(CHUNK_BLOCKS)
            hm = bytearray(256)
            for lx in range(16):
                for lz in range(16):
                    col = (bx + lx) * D + (bz + lz)
                    dst = (lx << 11) | (lz << 7)
                    s[dst:dst + H] = sky[col * H:col * H + H]
                    b[dst:dst + H] = blk[col * H:col * H + H]
                    hm[(lz << 4) | lx] = heights[col] & 0xFF
            c.sky, c.blocklight, c.heightmap = s, b, hm

    def light_at(self, x, y, z):
        """(sky, block) after compute_light()."""
        c = self.chunks[(x >> 4, z >> 4)]
        i = idx(x & 15, y, z & 15)
        return c.sky[i], c.blocklight[i]

    # -- serialisation ------------------------------------------------------
    def chunk_nbt(self, c):
        if c.sky is None:
            raise RuntimeError("compute_light() before serialising")
        tes = [c.tile_entities[k] for k in sorted(
            c.tile_entities, key=lambda p: (_TE_ORDER[child(
                c.tile_entities[p], "id").value], p))]
        for (x, y, z) in c.tile_entities:
            if (x >> 4, z >> 4) != (c.cx, c.cz):
                raise ValueError("tile entity outside its chunk")
        # A chunk with mobs is written as a pure-Beta chunk: no
        # CaveWiiTileEntitiesComplete marker. The marker's mere PRESENCE makes
        # the game's save merge "native" (region_archive.c: native = original
        # has the tag; entity_archive.c chunk_entities_preserve), which can only
        # delete a departed row that is byte-identical to CaveWii's own dump,
        # so the first save after a mob walks would fail (CEP_NATIVE_UNSAFE).
        # Without it, the strict Beta import path owns the rows and a moved mob
        # saves; the game writes its own canonical rows from then on.
        marker = ([] if c.entities
                  else [("CaveWiiTileEntitiesComplete", Byte(1))])
        level = Compound([
            ("Blocks", ByteArray(c.blocks)),
            ("Data", ByteArray(pack_nibbles(c.meta))),
            ("SkyLight", ByteArray(pack_nibbles(c.sky))),
            ("BlockLight", ByteArray(pack_nibbles(c.blocklight))),
            ("HeightMap", ByteArray(c.heightmap)),
            ("Entities", List(TAG_COMPOUND, c.entities)),
            ("TileEntities", List(TAG_COMPOUND, tes)),
        ] + marker + [
            ("LastUpdate", Long(0)),
            ("xPos", Int(c.cx)),
            ("zPos", Int(c.cz)),
            ("TerrainPopulated", Byte(1)),
            ("CaveWiiWallSignMetadataVersion", Byte(WALL_SIGN_META_VERSION)),
        ])
        return encode_root(Compound([("Level", level)]))

    def level_dat_nbt(self):
        px, py, pz = self.player_pos
        player = Compound([
            # Beta stores half-hearts (EntityLiving.java:558, full = 20); the
            # exact CaveWii units ride in CaveWiiHealth (level_archive.c).
            ("Health", Short(FULL_HEALTH // 8)),
            ("CaveWiiHealth", Short(FULL_HEALTH)),
            ("AttackTime", Short(0)), ("HurtTime", Short(0)),
            ("DeathTime", Short(0)), ("Air", Short(300)), ("Fire", Short(-20)),
            ("FallDistance", Float(0.0)), ("Dimension", Int(0)),
            ("OnGround", Byte(1)),
            ("Pos", List(TAG_DOUBLE, [Double(px), Double(py + PLAYER_EYE_HEIGHT),
                                      Double(pz)])),
            ("Rotation", List(TAG_FLOAT, [Float(self.player_yaw), Float(0.0)])),
            ("Motion", List(TAG_DOUBLE, [Double(0), Double(0), Double(0)])),
            ("Inventory", self._inventory_tag()),
        ])
        data = Compound([
            ("RandomSeed", Long(self.seed)), ("Time", Long(self.time)),
            ("LastPlayed", Long(0)), ("SizeOnDisk", Long(0)),
            ("SpawnX", Int(self.spawn[0])), ("SpawnY", Int(self.spawn[1])),
            ("SpawnZ", Int(self.spawn[2])),
            ("thundering", Byte(0)), ("raining", Byte(1 if self.raining else 0)),
            ("rainTime", Int(self.rain_time)), ("thunderTime", Int(0)),
            ("version", Int(19132)),
            ("CaveWiiGenerator", Int(self.generator)),
            ("CaveWiiSaveVersion", Int(SAVE_VERSION)),
            ("LevelName", String(self.level_name)),
            ("Player", player),
        ])
        return encode_root(Compound([("Data", data)]))

    def _inventory_tag(self):
        """Player slots: 0-8 hotbar, 9-35 main, 100-103 armour (Beta
        InventoryPlayer.writeToNBT; item_nbt.c runtime_slot)."""
        items = []
        seen = set()
        for it in self.inventory:
            slot = it[0]
            if not (0 <= slot <= 35 or 100 <= slot <= 103) or slot in seen:
                raise ValueError("bad/duplicate player slot %r" % slot)
            seen.add(slot)
            items.append(item_tag(slot, it[1], it[2], it[3] if len(it) > 3 else 0))
        return List(TAG_COMPOUND, items)

    def save(self, save_dir, light=True, zlib_level=6, overwrite=True):
        """Write <save_dir>/level.dat and <save_dir>/region/r.X.Z.mcr.
        Refuses to write into a directory that already holds a different
        world unless overwrite=True (then stale region files are removed)."""
        if light:
            self.compute_light()
        region_dir = os.path.join(save_dir, "region")
        os.makedirs(region_dir, exist_ok=True)
        files = pack_regions(self, zlib_level)
        existing = [f for f in os.listdir(region_dir)]
        if existing and not overwrite:
            raise FileExistsError(region_dir)
        for f in existing:
            if f not in files:
                os.remove(os.path.join(region_dir, f))
        for name, blob in files.items():
            _write_atomic(os.path.join(region_dir, name), blob)
        _write_atomic(os.path.join(save_dir, "level.dat"),
                      gzip.compress(self.level_dat_nbt(), mtime=0))
        return sorted(files)


# Tile-entity order inside one chunk = tile_entities_build's kind order.
_TE_ORDER = {"Chest": 0, "Sign": 1, "Furnace": 2, "Trap": 3, "Music": 4,
             "RecordPlayer": 5, "MobSpawner": 6, "Piston": 7}


def _write_atomic(p, blob):
    tmp = p + ".tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, p)


def region_file_name(cx, cz):
    return "r.%d.%d.mcr" % (cx >> 5, cz >> 5)


def pack_region(chunk_records, timestamp=1):
    """chunk_records: {(local_x, local_z): zlib-compressed chunk NBT}.
    Returns the .mcr bytes (chunks laid out in z-major slot order)."""
    offsets = bytearray(SECTOR)
    stamps = bytearray(SECTOR)
    body = bytearray()
    next_sector = 2
    for (lx, lz) in sorted(chunk_records, key=lambda p: (p[1], p[0])):
        payload = chunk_records[(lx, lz)]
        rec = struct.pack(">IB", len(payload) + 1, 2) + payload
        sectors = (len(rec) + SECTOR - 1) // SECTOR
        if sectors >= 256:
            raise ValueError("chunk (%d,%d) needs %d sectors (max 255)"
                             % (lx, lz, sectors))
        rec += b"\x00" * (sectors * SECTOR - len(rec))
        k = (lx + lz * 32) * 4
        struct.pack_into(">I", offsets, k, (next_sector << 8) | sectors)
        struct.pack_into(">I", stamps, k, timestamp & 0x7FFFFFFF)
        body += rec
        next_sector += sectors
    return bytes(offsets + stamps + body)


def pack_regions(world, zlib_level=6):
    regions = {}
    for (cx, cz), c in world.chunks.items():
        payload = zlib.compress(world.chunk_nbt(c), zlib_level)
        regions.setdefault(region_file_name(cx, cz), {})[(cx & 31, cz & 31)] = payload
    return {name: pack_region(recs, world.timestamp)
            for name, recs in regions.items()}


# ------------------------------------------------------------------ reading --
def read_region(data):
    """Parse .mcr bytes -> {(local_x, local_z): (root Tag, timestamp)}."""
    if len(data) < 2 * SECTOR:
        raise ValueError("region shorter than its header")
    out = {}
    for k in range(1024):
        (entry,) = struct.unpack_from(">I", data, k * 4)
        if not entry:
            continue
        (stamp,) = struct.unpack_from(">I", data, SECTOR + k * 4)
        off, cnt = entry >> 8, entry & 0xFF
        start = off * SECTOR
        length, ctype = struct.unpack_from(">IB", data, start)
        if ctype != 2 or length - 1 > cnt * SECTOR - 5:
            raise ValueError("bad record for slot %d" % k)
        raw = zlib.decompress(data[start + 5:start + 4 + length])
        out[(k % 32, k // 32)] = (decode_root(raw)[1], stamp)
    return out


def load_world(save_dir):
    """Read a save back into a World (blocks, metadata, light, TEs). Used by
    preview.py and the tests; also works on saves the game wrote."""
    with open(os.path.join(save_dir, "level.dat"), "rb") as f:
        _, root = decode_root(gzip.decompress(f.read()))
    data = child(root, "Data")
    name = child(data, "LevelName")
    w = World(name.value if name else os.path.basename(save_dir),
              seed=child(data, "RandomSeed").value,
              spawn=(child(data, "SpawnX").value, child(data, "SpawnY").value,
                     child(data, "SpawnZ").value))
    rdir = os.path.join(save_dir, "region")
    for fn in sorted(os.listdir(rdir)):
        if not fn.endswith(".mcr"):
            continue
        with open(os.path.join(rdir, fn), "rb") as f:
            recs = read_region(f.read())
        for (_, _), (tag, _) in recs.items():
            lv = child(tag, "Level")
            c = w.chunk(child(lv, "xPos").value, child(lv, "zPos").value)
            c.blocks[:] = child(lv, "Blocks").value
            c.meta[:] = unpack_nibbles(child(lv, "Data").value)
            c.sky = unpack_nibbles(child(lv, "SkyLight").value)
            c.blocklight = unpack_nibbles(child(lv, "BlockLight").value)
            c.heightmap = bytearray(child(lv, "HeightMap").value)
            ents = child(lv, "Entities")
            c.entities = list(ents.value if ents else [])
            tes = child(lv, "TileEntities")
            for te in (tes.value if tes else []):
                c.tile_entities[(child(te, "x").value, child(te, "y").value,
                                 child(te, "z").value)] = te
    return w
