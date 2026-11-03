#!/usr/bin/env python3
"""
Build the shipped CaveWii tutorial world ("Tutorial Island": island trail +
hub) from tmp/jobs/tutorial-1021/layout-spec.md (layout-spec v1).

    python tools/tutorial_world/build_tutorial.py            # saves/CavEX Tutorial
    python tools/tutorial_world/build_tutorial.py --out DIR [--previews DIR]

V2 style pass (tmp/jobs/tutorial-1021/v2-style-guide.md): medieval harbour
town with a castle hub, Beta 1.7.3 palette only. Half-timbered houses with
jettied upper storeys (the ground floor of every stop is unchanged), castle
keep + lab tower, fountain plaza with market stalls, market street, windmill,
harbour pier + lighthouse headland, giant CAVEX wordmark on the mine hill, wool
pixel art, curved paths, rolling meadows, mixed woodland. The V2 passes run
after every stop and only write into free grass/air or cells they own; the
circuits, gates, signs and chests are byte-identical to draft 1.

V3 pass (tmp/jobs/tutorial-1021/v3-plan.md): one 3-wide trail spine (gravel
centre, cobble kerbs) with numbered stop gateways, stop-colour stripes, inlaid
glowstone-tipped arrows and plain forecourts; a castle hill (keep on a cobble
plinth with 5x5 towers and a recessed gatehouse) with terraces, a lowered
mine quarry with benches, a north stone cliff and a varied coast; a chunky
3-deep CAVEX on stone shoulders (open-counter A, no face); varied town
silhouettes, a tapered windmill, a harbour ship and quay crane, a "you are
here" wool map, a four-way hub signpost; lanterns, wall torches, floor glow
and jack-o'-lanterns instead of fence-torch posts. Vertical moves are rigid
column shifts, so circuits keep their exact relative geometry.

V6 pass (tmp/jobs/tutorial-1021/v6-plan.md): legible order (a big wool
numeral on every gate and hub exit, 1..15; a colour key wall beside a flat
island map under glass at the landing), a sandstone spine with flush cobble
kerbs and 5x5 dark wool arrows (gravel only in the quarry), a hub that
points at the next exit (11), stops that show their lesson (9x9 farm field,
pen, barn; mine adit, ore at step height, lip shed, headframe; practice
bays and a boarding platform; an open-front workshop and a low cottage with
a garden), half the freestanding lamps and pumpkin posts gone, banners for
the castle crest, a slimmer CAVEX sign hung over the west sea (its shadow
falls on water) and a trail end (15) at the lighthouse.

V6b pass (tmp/jobs/tutorial-1021/v6/audit/AUDIT.md, every fix): crafting
tables where a lesson needs one (stops 7, 10, the hall's build pad); doors
only where they open onto a free cell (bed nooks that are only a bed are
open alcoves; stop 4's nook is 2x2; stop 6's door hangs in its doorway);
three town houses furnished for their use (each with a hearth under its
chimney); a working windmill floor and sails raised clear of the door;
market goods on counters; fence-row bay dividers; no glowstone lattices in
lawns, floors or parapets (fence torches, lantern pillars, wall torches,
ceiling lights and canopy lamps instead); no dark spawn platform on a roof,
awning, deck, ramp coping, sail or tunnel top; no floating overhang, lamp or
roof cap (hip roofs now carry each ring on a solid ring); no one-block
pimples. V6c: the pen holds 2 sheep (one pink), 2 cows, 2 pigs and 2
chickens on grass, with a door, a water trough and fence-post torches: see
stop8_farm and pen_animals.

Deterministic: no randomness; decoration uses a fixed integer hash of (x, z).
One function per stop / zone (stop1_beach .. islet) so a critique edit is a
local change. Section numbers in comments refer to layout-spec.md.

Checks run on every build (build() raises on any failure):
  * check_problems(w) == []  -- World.problems() plus this file's own checks
    (see check_problems for the two library false positives it filters and
    re-verifies: torch on a fence, MobSpawner tile entity);
  * no dark spawn spot (World.dark_spawn_spots) on any walkable cell
    (2.16: roads, building floors, plaza, yards, corridor, causeway, quarry,
    spiral, gallery); crop/sapling cells +1 have block light >= 9;
  * leaves within 4 leaf-steps of a log (BlockLeaves.java:56);
  * the monster lab's dark cell stays dark and its lit cell lit;
  * sign lines <= 15 chars, uppercase; every chest non-empty;
  * block ids <= 96; <= 2 spawners; no road cell inside a building footprint.

SPEC DEVIATIONS: the spec contradicts itself in places (coordinates that put a
road through a wall, a circuit that cannot work under the decompile rules).
Each fix is listed in DEVIATIONS below with its reason; build() prints them.
"""
import argparse
import hashlib
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import worldgen_lib as wl  # noqa: E402
import light_tables  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_OUT = os.path.join(REPO, "saves", "CavEX Tutorial")
DEFAULT_PREVIEWS = os.path.join(REPO, "tmp", "jobs", "tutorial-1021")

# Beta item ids used by the chests (Item.java shiftedIndex = 256 + n).
APPLE, BOW, ARROW, COAL, IRON_INGOT = 260, 261, 262, 263, 265
WOOD_SWORD, WOOD_SHOVEL, WOOD_PICK, WOOD_AXE = 268, 269, 270, 271
STONE_PICK, STICK, WOOD_HOE, SEEDS, WHEAT, BREAD = 274, 280, 290, 295, 296, 297
WOOD_DOOR_ITEM, BUCKET, MINECART, REDSTONE, BONE_MEAL = 324, 325, 328, 331, 351
BED_ITEM, REPEATER_ITEM = 355, 356
# V7 (each id checked against mcb173-retromcp Item.java, 256 + n).
IRON_PICK, FLINT_STEEL, DIAMOND_PICK, GOLD_PICK = 257, 259, 278, 285
STONE_SWORD, BOWL, STEW, PORK_RAW, PORK_COOKED = 272, 281, 282, 319, 320
LEATHER_ARMOUR = (298, 299, 300, 301)   # helmet, chestplate, leggings, boots
IRON_ARMOUR = (306, 307, 308, 309)
LEATHER, BOAT, REED_ITEM, PAPER, BOOK = 334, 333, 338, 339, 340
COMPASS, FISHING_ROD, CLOCK, FISH_COOKED = 345, 346, 347, 350
GOLD_INGOT = 266
# Item ids whose Beta max stack is 1 (ItemFood, ItemTool, ItemSword, ItemHoe,
# ItemArmor, ItemBow, ItemBucket, ItemMinecart, ItemBoat, ItemSign,
# ItemDoor, ItemFlintAndSteel, ItemFishingRod, ItemSaddle, ItemMap,
# ItemShears, cake, bed, records: maxStackSize = 1). Cookie 8, egg and
# snowball 16, everything else 64.
STACK_1 = ({256, 257, 258, 259, 260, 261} | set(range(267, 280))
           | {282} | set(range(283, 287)) | set(range(290, 295)) | {297}
           | set(range(298, 318)) | {319, 320, 322, 323, 324, 325, 326, 327,
                                    328, 329, 330, 333, 335, 342, 343, 346,
                                    349, 350, 354, 355, 358, 359, 2256, 2257})
STACK_MAX = {357: 8, 332: 16, 344: 16}

FENCE, TORCH, SLAB, GRAVEL, PLANKS, COBBLE = 85, 50, 44, 13, 5, 4
OBSIDIAN, LAVA, GLASS, CACTUS, SAND = 49, 11, 20, 81, 12
# BlockFire.java init: blocks that can catch fire (planks, slabs are rock
# in 1.7.3, fence, wooden stairs, log, leaves, bookshelf, TNT, tall grass,
# wool).
FLAMMABLE = {5, 17, 18, 31, 35, 46, 47, 53, 85}


def max_stack(item_id):
    if item_id in STACK_1:
        return 1
    return STACK_MAX.get(item_id, 64)

DEVIATIONS = [
    "shed kit: walls stand ON the footprint perimeter (not set back): every "
    "interior coordinate in section 2 (tables, chests, nooks) assumes it",
    "stop 1: demo lantern post moved from the stair cell (200,296) to "
    "(202,296); its sign hangs on the post at (202,66,297)",
    "stop 2: fallen-log gate moved from (199..201,262) (not on any road) to "
    "L2 at (193..195,66,262); nursery torch post (200,279) was on L1 -> (197,279)",
    "stop 2: nook bed foot (207,272) head (207,271) (spec put the head in the "
    "nook wall); chest lean-to x203..205 so it does not overlap the nook",
    "stop 3: nook door at (187,66,247) (the spec cell (187,248) is the chest's "
    "neighbour); chest stays inside the nook at (186,66,248)",
    "stop 4: L4 ran through the shed; it now branches off L3 at (202,227) and "
    "goes round the south side; gate wall moved to x=217 on the z=222 leg; "
    "chest moved to (205,66,224) because the SE nook needs (209,224)",
    "stop 5: L5 ran through the hut's south wall and the Redstone hall; it now "
    "leaves the hut's east doorway (stone plug IN the doorway at (233,229)) and "
    "runs (238,229)->(238,224)->(256,224)->(256,206); beacon post moved to "
    "(226,226) because its east face was the hut wall",
    "stop 5: quarry face (z 213..215) raised to y=73 so all three ore bands "
    "exist (the hill is only 69-70 high at z=215)",
    "stop 6/7: torch tunnel shortened to x 260..276 (spec x 260..284 overlapped "
    "the smelting shed and the L6 bend at x=282); porch x 277..281",
    "stop 6: chest (257,66,199); a 5x5 shed has a 3x3 interior, so V6b drops "
    "the nook partition (it left one standing cell) and hangs the door in "
    "the doorway (256,66,202)",
    "stop 7: chimney = 2 double slabs + 1 slab (3 stacked single slabs leave "
    "half-block gaps); gate sign in the air cell (295,67,213) facing west",
    "stop 8: no gravel inside the farm yard (the spec road crossed the demo "
    "row at x=313); L7 ends at the west gap; the south gate is reeds on "
    "(313,250) and (315,250) around a water source at (314,65,250) (a reed "
    "needs water next to its own soil, BlockReed.java:36, so the spec's 3 reeds "
    "+ water at x=316 could not stand); practice rows z 248..249",
    "stop 9: L8 ends at the front doorway (314,268); L9 starts outside the back "
    "doorway and runs (314,276)->(314,278)->(296,278)->(296,294)",
    "stop 10: demo bed foot (294,66,297) head (294,296) (spec head was in the "
    "north wall)",
    "hub: added a WEST plaza entrance (LH needs one) and an EAST road LE "
    "(269,256)->(300,256)->(300,241) to the bridge branch; the bridge sign "
    "stands at the east entrance",
    "3.1 clock: spec feed and return wires were adjacent rows, which Beta "
    "connects (BlockRedstoneWire connection rules) -> short circuit. Rebuilt: "
    "return row two rows south (z=230) and a delay-1 repeater facing north "
    "into the inverter block; whole clock shifted +1 x so a START button fits "
    "on the inverter's west face (a written clock has no pending tick)",
    "3.2 ripple: spec pistons at y=64 are 3 blocks from every wire (Beta "
    "notifies only Manhattan <= 2), so they would never update; pistons now at "
    "y=65 in the floor with the wool tile on top (y=66), STICKY (a normal "
    "piston leaves the tile up), line moved to x 256..265 (spec x 258..267 "
    "ran into the hall's east wall)",
    "3.3 bridge: pit sign in the exit well at (323,65,214) facing north (spec "
    "cell (323,65,215) is a walkway top with sea behind it)",
    "3.4 carts: only ONE powered rail per station (208, 238): with two, the "
    "detector rail beside the inner one lit it (cart ping-pongs) and a cart "
    "braking to a stop on the inner rail has no block behind it to launch "
    "from (EntityMinecart.java:403-414); E-end fence omitted (it sat in the "
    "gate gap); shelters on 4 posts around the station",
    "monster lab: cells shifted 1 east (spec dark cell interior began in the "
    "room's west wall): dark x249..253, glass x=254, lit x259..263, glass "
    "x=258; the strip behind the cells (z=283) is filled solid",
    "Redstone hall / lab roofs: full 45-degree stair gables (ridge y=78/77) "
    "instead of 'ridge y=73', which a stair roof over 17/15 blocks cannot reach",
    "V3 hill: keep + plaza + lab lifted +6, cart yard, town and three grass "
    "terraces +3, as rigid column shifts (blocks, metadata, tile entities); "
    "every circuit is byte-identical relative to its new floor (V2->V3 diff "
    "of the clock, ripple, cart track, lab cells: 0 changed cells)",
    "V3 mine: quarry, face, tunnel, hut, spiral, gallery and ladder lowered 3 "
    "as one rigid body ('downhill to the mine'); ore depths stay truthful "
    "(coal 67-68, iron 65-66 and 38-39, deep ores at 10-11)",
    "V3 roads: L10 now turns north at x=245 and enters the plaza at its "
    "south-west corner up a 6-step ramp (the z 266..268 west entrance is "
    "closed); LS extended to x=244 to meet it; cobble-stair ramps join the "
    "tiers on L4, L5, LH, LE (twice) and L10",
    "V3 cart yard: stone platform + grass instead of a gravel field; the two "
    "station note blocks keep gravel under them, because the instrument is "
    "chosen by the material below (TileEntityNote.java:32-46)",
    "V6 CAVEX: the plan asked for ~20 blocks seaward; the only water both "
    "under the sign and inside the Wii's 5-chunk near view of spawn AND the "
    "plaza's west gate is the west sea, so the sign hangs there (letter "
    "plane x=162 since V8, z 226..281) and faces east toward the island; spawn yaw "
    "turned to 145 so gate 1 and the sign share the opening view",
    "V6 gate 1 moved from (200,299) to (200,293) on L1: 5 blocks from spawn "
    "its numeral board filled the view and hid the approach",
    "V6 farm: the yard's east gap is fenced (it led nowhere; the pen stands "
    "there); V6b: the pen's gap is a wooden door (no fence gate in 1.7.3) "
    "and its sign says it is for the player's animals",
    "V6c pen: 8 animals written as exact Beta 1.7.3 mob rows "
    "(worldgen_lib.mob, cited to the decompile). Their chunk (19,15) is a "
    "pure-Beta chunk (no CaveWiiTileEntitiesComplete marker): in a native "
    "chunk a moved Beta mob fails the next save (CEP_NATIVE_UNSAFE), while "
    "the strict Beta import path saves it; proven by "
    "tests/test_tutorial_world_load.c check_animals (move, save, reload x2). "
    "The pen grows south to z 246 so 8 animals have room; the slab trough "
    "becomes a sunk water trough",
    "V6b yard: the lawn lattice is replaced by fence torches, south-fence "
    "lantern pillars and platform-edge lanterns (not by glowstone under "
    "the booster torches: the platform lanterns light it with the track "
    "untouched)",
    "V6 yard: the east station gets a flat slab canopy so the view west "
    "from exit 12 to the sign stays open; circuits and track unchanged",
]


# Stop 8 animal pen: fence ring (x1, z1, x2, z2) at y 66, and its animals
# (cell x, cell z, Beta mob id, worldgen_lib.mob options), feet at y 66.
PEN_BOX = (316, 238, 321, 246)
PEN_ANIMALS = (
    (318, 240, "Sheep", {}),
    (319, 242, "Sheep", {"color": "pink"}),
    (318, 243, "Cow", {"yaw": 90.0}),
    (319, 245, "Cow", {"yaw": 200.0}),
    (317, 244, "Pig", {}),
    (319, 240, "Pig", {"yaw": 45.0}),
    (317, 242, "Chicken", {}),
    (318, 245, "Chicken", {"yaw": 300.0}),
)


def H(x, z, salt=0):
    """Deterministic 32-bit hash of a column (decoration only)."""
    h = (x * 73856093) ^ (z * 19349663) ^ (salt * 83492791)
    h = (h ^ (h >> 13)) * 1274126177
    return (h ^ (h >> 16)) & 0xFFFFFFFF


def rnd(v):
    return int(math.floor(v + 0.5))


def solid(bid):
    return light_tables.light_of(bid)[1] == 255


def split_sign(text):
    return [s.strip() for s in text.split("/")]


class Tutorial:
    def __init__(self):
        # Seed = biome map only (every chunk is written, nothing generates).
        # 20261021 put the island in taiga/tundra: Beta's weather pass
        # (World.updateBlocksAndPlayCaveSounds) froze the sea to ice for
        # hours -- notify storm, remesh, a dirty-chunk save every autosave
        # (Wii fps dips). 20261024 has no snow biome over x,z -32..543.
        self.w = wl.World("Tutorial Island", seed=20261024,
                          spawn=(200, 65, 304), time=1000,
                          player_pos=(200.5, 65.0, 304.5), player_yaw=145.0)
        # V6: the player faces north-west by north: the boardwalk and gate 1
        # ahead to the right, the CAVEX sign up ahead over the west sea (yaw
        # 145 = 35 deg west of north; Beta yaw 180 = facing -z).
        self.road = {}          # (x, z) -> floor y of the road surface
        self.road_order = []
        self.walk = set()       # (x, y, z) feet cells that must stay lit
        self.growth = set()     # (x, y, z) cells that need block light >= 9
        self.footprints = {}    # name -> (x1, z1, x2, z2) enclosed buildings
        self.zones = {}         # name -> (x1, z1, x2, z2) open feature areas
        self.soft = {}          # stop boxes that only keep decoration out
        self.reserved = set()   # (x, z) decoration must avoid
        self.signs = []
        self.chests = []
        self.spawners = []
        self.fix_torches = []
        self.dropped_trees = []
        self.moved_trees = []
        self.lamp_posts = 0
        self.house_meta = {}    # name -> shed() parameters (window pass)
        self.sealed = []        # (x1, z1, x2, z2, y1, y2) closed rooms to light
        self.upper_boxes = {}   # name -> (x1, z1, x2, z2, band y) upper storeys
        self.no_window = set()  # (x, y, z) wall cells the window pass skips
        self.kerb = {}          # V6: (x, z) -> y of a flush cobble kerb cell
        self.n_floor_glow = 0   # isolated glowstone set flush in floors
        self.board_on_lintel = []   # V8: gates whose board had no side room

    # ------------------------------------------------------------ helpers --
    def ground(self, x, z):
        """Highest y whose block is opaque (the floor_y of section 1)."""
        c = self.w.chunks.get((x >> 4, z >> 4))
        base = wl.idx(x & 15, 0, z & 15)
        for y in range(wl.WORLD_HEIGHT - 1, 0, -1):
            if solid(c.blocks[base + y]):
                return y
        return 0

    def bid(self, x, y, z):
        return self.w.get(x, y, z)[0]

    def sign(self, x, y, z, text, facing, wall=False):
        lines = split_sign(text) if isinstance(text, str) else list(text)
        for ln in lines:
            assert ln == ln.upper(), "sign text must be uppercase: %r" % ln
        self.w.sign(x, y, z, lines, facing=facing, wall=wall)
        self.signs.append(((x, y, z), lines))

    def post_sign(self, x, y, z, text, facing):
        """Standing sign on a planks block (2.0: never on a fence)."""
        self.w.set(x, y - 1, z, PLANKS)
        self.sign(x, y, z, text, facing)

    def chest(self, x, y, z, items):
        assert items, "every chest holds something"
        for it in items:
            assert it[2] <= max_stack(it[1]), \
                "item %d x%d over its Beta stack size" % (it[1], it[2])
        self.w.chest(x, y, z, items)
        self.chests.append(((x, y, z), items))

    def spawner(self, x, y, z, mob="Zombie"):
        """MobSpawner TE exactly as tile_entities_nbt.c writes it
        ({id, x, y, z, EntityId, Delay(short)}; region_archive.c:7694)."""
        self.w.set(x, y, z, 52)
        self.w.set_tile_entity(x, y, z, wl.Compound([
            ("id", wl.String("MobSpawner")), ("x", wl.Int(x)), ("y", wl.Int(y)),
            ("z", wl.Int(z)), ("EntityId", wl.String(mob)),
            ("Delay", wl.Short(20))]))
        self.spawners.append((x, y, z))

    def lamp(self, x, z, y=None):
        """V3 cobble lantern at the ground of (x, z): cobble pedestal,
        glowstone lamp, stone slab cap (replaces the V2 fence + torch post;
        light 15 at fy+2, so it covers more than a torch)."""
        fy = self.ground(x, z) if y is None else y
        self.w.set(x, fy + 1, z, COBBLE)
        self.w.set(x, fy + 2, z, 89)
        self.w.slab(x, fy + 3, z, "stone")
        self.reserved.add((x, z))
        self.lamp_posts += 1

    def beacon(self, x, z, sign_side, text, wood="oak", fy=None):
        """Log x2 + jack-o-lantern (2.0 beacon post); wall sign on the
        `sign_side` face at floor_y+2, the lantern faces the same way."""
        fy = self.ground(x, z) if fy is None else fy
        self.w.log(x, fy + 1, z, wood)
        self.w.log(x, fy + 2, z, wood)
        self.w.pumpkin(x, fy + 3, z, sign_side, lit=True)
        dx, dz = wl.DIRS[sign_side]
        if text:
            self.sign(x + dx, fy + 2, z + dz, text, sign_side, wall=True)
        self.reserved.update({(x, z), (x + dx, z + dz)})

    def walk_rect(self, x1, z1, x2, z2, y):
        for x in range(min(x1, x2), max(x1, x2) + 1):
            for z in range(min(z1, z2), max(z1, z2) + 1):
                self.walk.add((x, y, z))

    def foundation(self, x1, z1, x2, z2, fy, top=PLANKS, top_meta=0,
                   fill=3):
        """Flat floor at fy over the box; supports down to the terrain and
        clears everything above (buildings sit on the terrain)."""
        for x in range(x1, x2 + 1):
            for z in range(z1, z2 + 1):
                g = self.ground(x, z)
                for y in range(min(g, fy) + 1, fy):
                    self.w.set(x, y, z, fill)
                for y in range(fy + 1, min(fy + 14, 127)):
                    self.w.set(x, y, z, 0)
                self.w.set(x, fy, z, top, top_meta)
                self.reserved.add((x, z))

    def gable(self, x1, z1, x2, z2, y0, ridge="x", material="wood",
              ridge_block=(PLANKS, 0), gable_block=(PLANKS, 0),
              overhang=1, ridge_cap=None):
        """Stair gable (2.0 roof rule) starting at y0 (eave). ridge='x':
        ridge line runs along x, roof slopes toward -z/+z; 'z' the other way."""
        if ridge == "x":
            a1, a2, b1, b2 = x1, x2, z1, z2
        else:
            a1, a2, b1, b2 = z1, z2, x1, x2
        depth = b2 - b1 + 1
        half = (depth - 1) // 2

        def put(a, b, y, blk, meta=0):
            if ridge == "x":
                self.w.set(a, y, b, blk, meta)
            else:
                self.w.set(b, y, a, blk, meta)
        up_lo, up_hi = ("south", "north") if ridge == "x" else ("east", "west")
        sid = 53 if material == "wood" else 67
        for k in range(half):
            y = y0 + k
            for a in range(a1 - overhang, a2 + overhang + 1):
                put(a, b1 + k, y, sid, wl.STAIRS_META[up_lo])
                put(a, b2 - k, y, sid, wl.STAIRS_META[up_hi])
            for b in range(b1 + k + 1, b2 - k):
                put(a1, b, y, *gable_block)
                put(a2, b, y, *gable_block)
        y = y0 + half
        for a in range(a1 - overhang, a2 + overhang + 1):
            for b in range(b1 + half, b2 - half + 1):
                put(a, b, y, *ridge_block)
                if ridge_cap is not None:
                    put(a, b, y + 1, *ridge_cap)
        return y

    def shed(self, name, x1, z1, x2, z2, fy, wall=(PLANKS, 0), post="oak",
             wall_h=3, roof="wood", ridge="x", floor=(PLANKS, 0),
             doors=(), trim=None, lower=None, checker=False, mossy=False,
             ridge_block=(PLANKS, 0), gable_block=None, mid_posts=0,
             upper=None, windows=True, chimney=None, ridge_cap="auto",
             chimney_block=COBBLE):
        """Building kit (2.0 + V2 style): floor, corner log posts, perimeter
        walls, doorways (1x2 air), stair gable with a slab ridge cap.
        V2: `upper` = infill block of a half-timbered upper storey that
        jetties out 1 block on both eave sides (the ground floor, where every
        stop's interior lives, is unchanged); `roof=None` = flat roof (the
        caller builds battlements); `windows` = glass in the window pass;
        `chimney` = (x, z) cobble stack through the roof. Returns the ridge y
        (or the roof deck y for roof=None)."""
        self.footprints[name] = (x1, z1, x2, z2)
        self.house_meta[name] = {"fy": fy, "wall_h": wall_h, "upper": upper,
                                 "ridge": ridge, "windows": windows}
        self.foundation(x1, z1, x2, z2, fy, floor[0], floor[1])
        for x in range(x1, x2 + 1):
            for z in range(z1, z2 + 1):
                if x not in (x1, x2) and z not in (z1, z2):
                    continue
                corner = x in (x1, x2) and z in (z1, z2)
                along = (x - x1) if z in (z1, z2) else (z - z1)
                for y in range(fy + 1, fy + wall_h + 1):
                    if corner or (mid_posts and along % mid_posts == 0):
                        self.w.log(x, y, z, post)
                    elif checker and along % 3 == 0:
                        self.w.log(x, y, z, post)
                    elif lower is not None and y == fy + 1:
                        self.w.set(x, y, z, *lower)
                    elif mossy and (along + y) % 3 == 0:
                        self.w.set(x, y, z, 48)
                    else:
                        self.w.set(x, y, z, *wall)
        gb = gable_block or wall
        if ridge_cap == "auto":
            ridge_cap = (SLAB, 2) if roof == "wood" else (SLAB, 3)
        if roof is None:
            top = fy + wall_h + 1
        elif roof == "hip":
            top = self.hip(x1, z1, x2, z2, fy + wall_h + 1)
        elif upper is not None:
            top = self.upper_storey(name, x1, z1, x2, z2, fy + wall_h + 1,
                                    ridge, roof, upper, post, ridge_cap)
        else:
            top = self.gable(x1, z1, x2, z2, fy + wall_h + 1, ridge, roof,
                             ridge_block, gb, ridge_cap=ridge_cap)
        if chimney is not None:
            cx, cz = chimney
            for y in range(fy + wall_h + 1, top + 3):
                self.w.set(cx, y, cz, chimney_block
                           if (y + cx) % 2 else COBBLE)
            self.w.slab(cx, top + 3, cz, "cobblestone")
        for (dx, dz) in doors:
            for y in (fy + 1, fy + 2):
                self.w.set(dx, y, dz, 0)
            if trim is not None:
                if dz in (z1, z2):
                    cells = ((dx - 1, dz), (dx + 1, dz))
                else:
                    cells = ((dx, dz - 1), (dx, dz + 1))
                for (tx, tz) in cells:
                    self.w.wool(tx, fy + wall_h, tz, trim)
                self.w.log(dx, fy + wall_h, dz, post)
        for x in range(x1 + 1, x2):
            for z in range(z1 + 1, z2):
                self.walk.add((x, fy + 1, z))
        return top

    def hip(self, x1, z1, x2, z2, y0, material="wood"):
        """V3 hip roof: an eave ring of stairs one block out at y0-1, then
        stair rings on all four sides stepping in by one per layer from the
        wall line; the last 1-2 wide strip is planks with a slab cap.
        Returns the cap y."""
        w = self.w
        sid = "wood" if material == "wood" else "cobblestone"
        cap = (SLAB, 2) if material == "wood" else (SLAB, 3)
        flat = PLANKS if material == "wood" else COBBLE
        for x in range(x1 - 1, x2 + 2):
            w.stairs(x, y0 - 1, z1 - 1, "south", sid)
            w.stairs(x, y0 - 1, z2 + 1, "north", sid)
        for z in range(z1, z2 + 1):
            w.stairs(x1 - 1, y0 - 1, z, "east", sid)
            w.stairs(x2 + 1, y0 - 1, z, "west", sid)
        ax1, az1, ax2, az2, y = x1, z1, x2, z2, y0
        while True:
            if ax2 - ax1 <= 1 or az2 - az1 <= 1:
                for x in range(ax1, ax2 + 1):
                    for z in range(az1, az2 + 1):
                        w.set(x, y, z, flat)
                        w.set(x, y + 1, z, *cap)
                return y + 1
            for x in range(ax1, ax2 + 1):
                w.stairs(x, y, az1, "south", sid)
                w.stairs(x, y, az2, "north", sid)
            for z in range(az1 + 1, az2):
                w.stairs(ax1, y, z, "east", sid)
                w.stairs(ax2, y, z, "west", sid)
            # V6b: a solid ring right inside this one carries the next
            # layer (the V6 rings touched only at their edges, so the top
            # ring and cap floated over the attic)
            bx1, bz1, bx2, bz2 = ax1 + 1, az1 + 1, ax2 - 1, az2 - 1
            for x in range(bx1, bx2 + 1):
                for z in range(bz1, bz2 + 1):
                    if x in (bx1, bx2) or z in (bz1, bz2):
                        w.set(x, y, z, flat)
            ax1, az1, ax2, az2, y = ax1 + 1, az1 + 1, ax2 - 1, az2 - 1, y + 1

    def upper_storey(self, name, x1, z1, x2, z2, yb, ridge, roof, infill,
                     post, ridge_cap):
        """V2 half-timber upper storey: a jetty band at yb overhanging the
        eave sides by 1, 3 rows of infill framed by log posts every 3, and
        the stair gable on the wider box. The ground floor below is
        untouched. The attic is sealed, so it joins self.sealed (lit by the
        sealed-room pass: no dark spawn spot in a closed room)."""
        w = self.w
        ex, ez = 0, 0          # no jetty: the upper storey sits on the walls
        ux1, uz1, ux2, uz2 = x1 - ex, z1 - ez, x2 + ex, z2 + ez
        for x in range(ux1, ux2 + 1):
            for z in range(uz1, uz2 + 1):
                corner = x in (ux1, ux2) and z in (uz1, uz2)
                if corner:
                    w.log(x, yb, z, post)
                else:
                    w.set(x, yb, z, PLANKS)
                if not (x in (ux1, ux2) or z in (uz1, uz2)):
                    continue
                along = (x - ux1) if z in (uz1, uz2) else (z - uz1)
                for y in range(yb + 1, yb + 4):
                    if corner or along % 3 == 0:
                        w.log(x, y, z, post)
                    else:
                        w.set(x, y, z, *infill)
        top = self.gable(ux1, uz1, ux2, uz2, yb + 4, ridge, roof,
                         (PLANKS, 0) if roof == "wood" else (COBBLE, 0),
                         infill, ridge_cap=ridge_cap)
        self.sealed.append((ux1 + 1, uz1 + 1, ux2 - 1, uz2 - 1, yb + 1, top))
        self.upper_boxes[name] = (ux1, uz1, ux2, uz2, yb)
        return top

    # -------------------------------------------------------------- 1.3 ----
    def terrain(self):
        w = self.w
        cache = {}

        def col(runs):
            key = tuple(runs)
            b = cache.get(key)
            if b is None:
                a = bytearray(wl.WORLD_HEIGHT)
                for (y1, y2, bid) in runs:
                    for y in range(y1, y2 + 1):
                        a[y] = bid
                b = cache[key] = bytes(a)
            return b

        def table(r, grass_lim):
            if r <= grass_lim:
                return 65, [(0, 0, 7), (1, 61, 1), (62, 64, 3), (65, 65, 2)]
            if r <= 1.0:
                return 64, [(0, 0, 7), (1, 60, 1), (61, 64, 12)]
            if r <= 1.3:
                top = 63 - rnd(8 * (r - 1) / 0.30)
                runs = [(0, 0, 7), (1, top - 3, 1), (top - 2, top, 12)]
                if top < 63:
                    runs.append((top + 1, 63, 9))
                return top, runs
            return 55, [(0, 0, 7), (1, 53, 1), (54, 55, 12), (56, 63, 9)]

        def clamp01(v):
            return max(0.0, min(1.0, v))

        for x in range(512):
            for z in range(512):
                ux, uz = (x - 256) / 80.0, (z - 256) / 70.0
                rA = math.hypot(ux, uz)
                # V3 coast character (main island only). The south-west
                # harbour cove keeps the V2 beach (spawn, quay, reeds);
                # elsewhere the beach width varies 2-6 blocks, an ESE grass
                # bluff has no beach, and the north coast is a stone cliff.
                ang = math.atan2(uz, ux)
                k_sw = clamp01((math.cos(ang - 2.44) - 0.80) / 0.10)
                k_blf = clamp01((math.cos(ang - 0.45) - 0.85) / 0.10)
                k_n = clamp01((-math.sin(ang) - 0.75) / 0.15)
                gl = 0.925 + 0.045 * (0.5 + 0.5 * math.sin(3 * ang + 0.6))
                gl = 0.90 * k_sw + gl * (1 - k_sw)
                gl = gl * (1 - k_blf) + 0.995 * k_blf
                top, runs = table(rA, gl)
                if k_blf > 0.5 and 0.93 < rA <= gl:
                    # grass bluff: a 1-block raised rim at the shore
                    top, runs = 66, [(0, 0, 7), (1, 62, 1), (63, 65, 3),
                                     (66, 66, 2)]
                elif k_blf > 0.5 and 1.0 < rA <= 1.3:
                    runs = [(a, b, 13 if bid == 12 else bid)
                            for (a, b, bid) in runs]      # gravel shore
                if k_n > 0.3 and 0.80 < rA <= 1.08:
                    if rA <= 0.96:
                        h = rnd(k_n * 2.0 * (rA - 0.80) / 0.16)
                        top = 65 + h
                        runs = [(0, 0, 7), (1, top - 4, 1),
                                (top - 3, top - 1, 3), (top, top, 2)]
                    elif rA <= 1.02:
                        top = 65 + rnd(k_n * (2.0 + 0.8 * math.sin(x * 0.7)
                                              * math.cos(z * 0.9)))
                        runs = [(0, 0, 7), (1, top, 1)]   # bare rock edge
                    else:
                        runs = [(0, 0, 7), (1, 60, 1), (61, 63, 9)]
                        top = 60                       # sheer cliff foot
                rB = math.hypot((x - 344) / 14.0, (z - 212) / 12.0)
                if rB <= 1.3:
                    t2, r2 = table(rB, 0.85)
                    if t2 > top:
                        top, runs = t2, r2
                rH = math.hypot((x - 230) / 20.0, (z - 210) / 12.0)
                if rH <= 1.0:
                    fy = 65 + min(8, rnd(9 * (1 - rH)))
                    runs = [(0, 0, 7), (1, fy - 2, 1), (fy - 1, fy - 1, 3),
                            (fy, fy, 2)]
                rP = math.hypot((x - 180) / 13.0, (z - 301) / 10.0)
                rP *= 1.0 + 0.10 * math.sin(3.0 * math.atan2(z - 301, x - 180)
                                            + 0.7)
                if rP <= 1.0:
                    # V2 west headland (harbour arm, lighthouse): grass top,
                    # rocky rim, sheer stone cliff into the sea.
                    t = 65 + min(6, max(0, rnd(9 * (1 - rP) + 1.3 * math.sin(
                        x * 0.55) * math.cos(z * 0.45))))
                    if rP > 0.84 or (rP > 0.6 and H(x, z, 3) % 7 == 0):
                        runs = [(0, 0, 7), (1, t, 1)]
                    else:
                        runs = [(0, 0, 7), (1, t - 3, 1), (t - 2, t - 1, 3),
                                (t, t, 2)]
                if 312 <= x <= 330 and 209 <= z <= 215:
                    runs = [(0, 0, 7), (1, 54, 1), (55, 64, 12), (65, 65, PLANKS)]
                c = w.chunk(x >> 4, z >> 4)
                base = wl.idx(x & 15, 0, z & 15)
                c.blocks[base:base + wl.WORLD_HEIGHT] = col(runs)
        # Quarry face backing (deviation): z 213..215 to y=73, tapering north.
        for x in range(222, 239):
            for z in range(209, 216):
                want = 73 if z >= 213 else 73 - (213 - z)
                g = self.ground(x, z)
                if g < want:
                    for y in range(62, want - 1):
                        w.set(x, y, z, 1)
                    w.set(x, want - 1, z, 3)
                    w.set(x, want, z, 2)

    def check_frame(self):
        for (cx, cz), c in self.w.chunks.items():
            for lx in range(16):
                for lz in range(16):
                    base = wl.idx(lx, 0, lz)
                    col = c.blocks[base:base + 128]
                    assert col[0] == 7, "bedrock missing"
                    assert all(col[y] != 0 for y in range(1, 56)), \
                        "air below the sea floor at %d,%d" % (cx * 16 + lx, cz * 16 + lz)

    # --------------------------------------------------------- 2.0 roads ---
    LEGS = [
        # name, points, surface ('gravel' | 'planks')
        ("L0", [(200, 304), (200, 297)], "planks"),
        ("L1", [(200, 295), (200, 272)], "gravel"),
        ("L2", [(200, 272), (194, 272), (194, 246)], "gravel"),
        ("L3", [(194, 246), (202, 246), (202, 222)], "gravel"),
        ("L4", [(202, 227), (214, 227), (214, 222), (230, 222), (230, 226)],
         "gravel"),
        ("L5", [(234, 229), (238, 229), (238, 224), (256, 224), (256, 206)],
         "gravel"),
        ("L6", [(256, 206), (259, 206)], "gravel"),
        ("L6b", [(282, 206), (282, 214), (290, 214)], "gravel"),
        ("L7", [(290, 214), (300, 214), (300, 240), (303, 240)], "gravel"),
        ("L7b", [(300, 212), (311, 212)], "gravel"),
        ("L8", [(314, 251), (314, 268)], "gravel"),
        ("L9", [(314, 276), (314, 278), (296, 278), (296, 294)], "gravel"),
        ("L10", [(292, 298), (245, 298), (245, 269)], "gravel"),
        ("LS", [(202, 290), (244, 290)], "gravel"),
        ("LH", [(243, 256), (241, 256)], "gravel"),
        ("LE", [(269, 256), (300, 256), (300, 241)], "gravel"),
        ("LI", [(331, 212), (341, 212)], "gravel"),   # islet path
    ]
    # Footprints the roads must never enter (asserted, never auto-moved).
    PLANNED_FOOTPRINTS = {
        "s2_nook": (206, 270, 208, 273), "s3_shed": (184, 243, 192, 249),
        "s4_shed": (204, 219, 210, 225), "s5_hut": (227, 227, 233, 231),
        "s6_shed": (254, 198, 258, 202), "tunnel": (260, 204, 276, 208),
        "s7_shed": (284, 205, 292, 211), "s8_shed": (316, 232, 322, 236),
        "s9_cottage": (310, 269, 318, 275), "s10_house": (293, 295, 299, 301),
        "hall": (246, 226, 266, 242), "lab": (248, 270, 264, 284),
        "pavilion": (342, 210, 346, 214),
    }

    def leg_cells(self, pts):
        cells = []
        for i in range(len(pts) - 1):
            (ax, az), (bx, bz) = pts[i], pts[i + 1]
            assert ax == bx or az == bz, "legs are axis-aligned"
            ext_a = 1 if i > 0 else 0
            ext_b = 1 if i < len(pts) - 2 else 0
            if ax == bx:
                s = 1 if bz >= az else -1
                for z in range(az - s * ext_a, bz + s * ext_b + s, s):
                    for d in (-1, 0, 1):
                        cells.append((ax + d, z))
            else:
                s = 1 if bx >= ax else -1
                for x in range(ax - s * ext_a, bx + s * ext_b + s, s):
                    for d in (-1, 0, 1):
                        cells.append((x, az + d))
        return cells

    @staticmethod
    def centre_line(pts):
        """Centre cells of a leg in walking order (corners included once)."""
        out = []
        for i in range(len(pts) - 1):
            (ax, az), (bx, bz) = pts[i], pts[i + 1]
            n = max(abs(bx - ax), abs(bz - az))
            sx, sz = (bx > ax) - (bx < ax), (bz > az) - (bz < az)
            for t in range(0 if i == 0 else 1, n + 1):
                out.append((ax + sx * t, az + sz * t))
        return out

    def roads(self):
        w = self.w
        self.leg_centre = {name: self.centre_line(pts)
                           for name, pts, _ in self.LEGS}
        self.centre_cells = set()
        for cells in self.leg_centre.values():
            self.centre_cells.update(cells)
        for name, pts, surface in self.LEGS:
            for (x, z) in self.leg_cells(pts):
                for fname, (x1, z1, x2, z2) in self.PLANNED_FOOTPRINTS.items():
                    if x1 <= x <= x2 and z1 <= z <= z2:
                        raise AssertionError("road %s cell (%d,%d) inside %s"
                                             % (name, x, z, fname))
                if (x, z) in self.road:
                    continue
                fy = self.ground(x, z)
                if surface == "planks":
                    w.set(x, fy, z, PLANKS)
                else:
                    w.set(x, fy, z, GRAVEL)
                for y in range(fy + 1, fy + 4):
                    if w.get(x, y, z)[0] in (31, 37, 38):
                        w.set(x, y, z, 0)
                self.road[(x, z)] = fy
                self.road_order.append((x, z))
                self.reserved.add((x, z))
        # V8: one clear open approach from the landing to the castle hill:
        # LS east along z 290 and L10 north along x 245 into the plaza keep
        # their verges free of trees, bushes, flowers and meadow relief
        self.soft["approach_LS"] = (203, 286, 248, 294)
        self.soft["approach_L10"] = (240, 266, 250, 300)
        # L0s: three wooden stairs rising north onto the plateau (2.0).
        for x in (199, 200, 201):
            w.stairs(x, 65, 296, "north")
            self.reserved.add((x, 296))

    def lamp_posts_along_legs(self):
        """Fence+torch every 6 cells along each leg, alternating sides."""
        n = 0
        for name, pts, surface in self.LEGS:
            for i in range(len(pts) - 1):
                (ax, az), (bx, bz) = pts[i], pts[i + 1]
                horiz = az == bz
                length = abs(bx - ax) if horiz else abs(bz - az)
                for t in range(3, length + 1, 6):
                    if horiz:
                        cx = ax + (t if bx > ax else -t)
                        cz = az
                    else:
                        cx = ax
                        cz = az + (t if bz > az else -t)
                    side = 1 if n % 2 == 0 else -1
                    n += 1
                    for s in (side, -side):
                        px, pz = (cx, cz + 2 * s) if horiz else (cx + 2 * s, cz)
                        if self.lamp_site_ok(px, pz):
                            self.lamp(px, pz)
                            break

    def lamp_site_ok(self, x, z):
        if (x, z) in self.road or (x, z) in self.reserved:
            return False
        for (x1, z1, x2, z2) in list(self.footprints.values()) + list(self.zones.values()):
            if x1 - 1 <= x <= x2 + 1 and z1 - 1 <= z <= z2 + 1:
                return False
        g = self.ground(x, z)
        if self.bid(x, g, z) not in (2, 3, 12, 24, 1):
            return False
        return all(self.bid(x, y, z) in (0, 31, 37, 38) for y in (g + 1, g + 2, g + 3))

    def kerbs(self):
        """Stone slab on every grass cell 4-adjacent to a road (2.0)."""
        w = self.w
        for (x, z) in self.road_order:
            fy = self.road[(x, z)]
            for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, nz = x + dx, z + dz
                if (nx, nz) in self.road:
                    continue
                if any(x1 <= nx <= x2 and z1 <= nz <= z2 for (x1, z1, x2, z2)
                       in list(self.footprints.values()) + list(self.zones.values())):
                    continue
                if self.bid(nx, fy, nz) == 2 and self.bid(nx, fy + 1, nz) in (0, 31, 37, 38):
                    w.slab(nx, fy + 1, nz, "stone")
                    self.reserved.add((nx, nz))

    def walk_roads(self):
        for (x, z), fy in self.road.items():
            self.walk.add((x, fy + 1, z))

    # ------------------------------------------------------- 2.1 stop 1 ---
    def stop1_beach(self):
        w = self.w
        self.soft["stop1"] = (194, 298, 210, 310)
        # V6: no trees between spawn and the CAVEX sign / gate 1
        self.soft["spawn_view"] = (184, 280, 198, 303)
        # Sandstone quay, 3 wide along x at z=306 (y=64, over the shallows).
        for x in range(194, 211):
            for z in range(305, 308):
                w.set(x, 64, z, 24)
                for y in range(65, 70):
                    w.set(x, y, z, 0)
                g = self.ground(x, z)
                self.walk.add((x, 65, z))
                self.reserved.add((x, z))
        # V6: the two quay-end lantern posts and one lamp are gone (less
        # clutter; the west end now leads to the trail-end bridge and the
        # east end is the map walkway).
        self.lamp(199, 307, 64)
        self.walk.discard((199, 65, 307))
        # V6b: the landing chest stands with the cargo crates on the quay
        # (alone on the quay it read as a stray block)
        self.chest(197, 65, 307, [(0, APPLE, 1), (1, APPLE, 1), (2, APPLE, 1),
                                  (3, APPLE, 1)])
        self.post_sign(202, 65, 303, "WELCOME! / WALK NORTH: / GATE 1 IS / "
                       "STRAIGHT AHEAD", "west")
        self.reserved.add((202, 303))
        # Demo lantern post beside the L0s stairs; its sign faces the boardwalk.
        self.beacon(202, 296, "south",
                    "LOOK AROUND / WALK TO THE / LANTERN POST / TURN TO LOOK",
                    wood="birch", fy=64)
        # Jump step + sign post on top.
        w.set(203, 65, 302, 12)
        w.set(203, 64, 302, 24)
        self.sign(203, 66, 302, "JUMP UP HERE", "west")
        self.reserved.add((203, 302))
        # Skip sign at the LS junction.
        self.post_sign(202, 66, 288, "SKIP THE TRAIL / HUB THIS WAY / "
                       "TRAIL: NORTH", "south")
        self.reserved.add((202, 288))
        # Beach reed clump with its 1-block water pocket (1.4).
        w.water_source(206, 64, 299)
        w.reed(206, 65, 300, 2)
        w.reed(207, 65, 299, 2)
        w.reed(205, 65, 299, 2)
        self.reserved.update({(206, 299), (206, 300), (207, 299), (205, 299)})

    # ------------------------------------------------------- 2.2 stop 2 ---
    def tree(self, x, z, kind="oak", cut_west_of=None, fy=None):
        w = self.w
        fy = self.ground(x, z) if fy is None else fy
        y0 = fy + 1
        for y in range(y0, y0 + 5):
            w.log(x, y, z, kind)
        layers = [(y0 + 2, 2), (y0 + 3, 2), (y0 + 4, 1), (y0 + 5, 0)]
        for (y, r) in layers:
            for dx in range(-r, r + 1):
                for dz in range(-r, r + 1):
                    if r == 2 and abs(dx) == 2 and abs(dz) == 2:
                        continue
                    if (dx, dz) == (0, 0) and y < y0 + 5:
                        continue
                    if cut_west_of is not None and x + dx <= cut_west_of:
                        continue
                    if w.get(x + dx, y, z + dz)[0] == 0:
                        w.leaves(x + dx, y, z + dz, kind)
        self.reserved.add((x, z))

    def stop2_woods(self):
        w = self.w
        self.soft["stop2"] = (192, 264, 210, 280)
        self.beacon(203, 274, "west",
                    "PUNCH THE LOG / HOLD UNTIL IT / DROPS A LOG / PICK IT UP")
        # Cut-away demo tree: west half of the canopy omitted.
        self.tree(204, 276, "oak", cut_west_of=203)
        self.post_sign(202, 66, 276, "LOGS INSIDE / LEAVES OUTSIDE / "
                       "LEAVES NEED A / LOG WITHIN 4", "west")
        # Practice trees with a planks step beside each.
        self.tree(196, 268, "oak")
        w.set(197, 66, 268, PLANKS)
        self.tree(208, 282, "oak")
        w.set(207, 66, 282, PLANKS)
        self.reserved.update({(197, 268), (207, 282)})
        # Sapling nursery (oak, spruce, birch) + torch post (deviation).
        for i, kind in enumerate(("oak", "spruce", "birch")):
            x = 194 + 2 * i
            w.set(x, 65, 278, 2)
            w.sapling(x, 66, 278, kind)
            self.growth.add((x, 67, 278))
            self.reserved.add((x, 278))
        self.post_sign(196, 66, 280, "PLANT SAPLINGS / ON GRASS, GIVE / "
                       "THEM LIGHT", "north")
        self.reserved.add((196, 280))
        self.lamp(197, 279)
        # Gate: fallen oak across L2 (deviation: on the road). V6b: two logs
        # high, so it has to be broken (a 1-high log is just jumped).
        for x in (193, 194, 195):
            w.log(x, 66, 262)
            w.log(x, 67, 262)
        self.post_sign(196, 66, 263, "BREAK THIS LOG / TO GO ON", "south")
        self.reserved.add((196, 263))
        # V6b: a lantern beside the 2-high log gate lights both sides of it
        # (never a torch on the logs: it would drop when they are broken)
        self.lamp(191, 264, 65)
        # Bed nook lean-to x206..208, z270..273 (ridge along z). V6b: the
        # 1x2 nook is the bed, so it is an open bed alcove (a door here
        # opened straight onto the bed foot).
        self.shed("s2_nook", 206, 270, 208, 273, 65, ridge="z",
                  doors=[(207, 273)])
        w.bed(207, 66, 272, "north")
        w.torch(207, 67, 271, "south")
        # Chest lean-to, open to the west, backed by the nook's west wall.
        self.foundation(203, 271, 205, 273, 65)
        for z in (271, 273):
            for y in (66, 67, 68):
                w.log(203, y, z)
            for x in (204, 205):
                w.set(x, 66, z, PLANKS)
                w.set(x, 67, z, PLANKS)
                w.set(x, 68, z, PLANKS)
        for x in range(202, 206):
            for z in range(270, 275):
                w.stairs(x, 69, z, "east")
        w.set(205, 66, 272, 0)
        self.chest(205, 66, 272, [(0, 17, 8), (1, 6, 4), (2, APPLE, 1),
                                  (3, APPLE, 1)])
        w.torch(205, 68, 272, "west")
        self.walk.update({(203, 66, 272), (204, 66, 272)})

    # ------------------------------------------------------- 2.3 stop 3 ---
    def stop3_crafting(self):
        w = self.w
        self.shed("s3_shed", 184, 243, 192, 249, 65, doors=[(192, 246)],
                  trim="green", mid_posts=4, lower=(COBBLE, 0),
                  upper=(35, 0), chimney=(185, 245))
        self.beacon(196, 243, "west",
                    "CRAFT PLANKS / 1 LOG = 4 / 2 PLANKS = / 4 STICKS")
        w.set(186, 66, 244, 58)
        self.sign(186, 67, 244, "OPEN CRAFTING / MAKE PLANKS / THEN A TABLE / "
                  "PLACE IT HERE", "south", wall=True)
        # V6b: a wall sign above the spot, so the 'here' cell stays free
        self.sign(190, 67, 244, "PLACE YOUR / TABLE HERE", "south", wall=True)
        # Nook (SW): partition, door, bed, chest, torch.
        for x in (185, 186):
            for y in (66, 67, 68):
                w.set(x, y, 246, PLANKS)
        for y in (66, 67, 68):
            w.set(187, y, 248, PLANKS)
        w.set(187, 68, 247, PLANKS)
        w.door(187, 66, 247, "west")
        w.bed(185, 66, 248, "north")
        self.chest(186, 66, 248, [(0, 17, 8), (1, 58, 1)])
        w.torch(185, 67, 247, "east")
        w.torch(189, 68, 248, "north")
        w.torch(188, 68, 244, "south")
        # Gate: ditch across L3. V6b: 2 deep (a 1-deep ditch is walked in
        # and out of, so 'fill the ditch' was never needed).
        for x in (201, 202, 203):
            for z in (241, 242):
                w.set(x, 65, z, 0)
                w.set(x, 64, z, 0)
                self.walk.discard((x, 66, z))
                self.walk.add((x, 64, z))
        self.post_sign(204, 66, 241, "MAKE PLANKS / FILL THE DITCH / TO CROSS",
                       "south")
        self.lamp(205, 240)
        self.reserved.add((204, 241))

    # ------------------------------------------------------- 2.4 stop 4 ---
    def stop4_tools(self):
        w = self.w
        self.shed("s4_shed", 204, 219, 210, 225, 65, doors=[(204, 222)],
                  trim="green", lower=(COBBLE, 0), post="spruce",
                  upper=(24, 0), chimney=(209, 220))
        self.beacon(202, 218, "south", "MAKE TOOLS / PICKAXE FIRST / "
                    "THEN AXE AND / SHOVEL")
        w.set(207, 66, 220, 58)
        self.sign(206, 67, 220, "CRAFT STICKS / THEN A PICKAXE / AXE AND "
                  "SHOVEL / TAKE THEM ALL", "south", wall=True)
        self.sign(208, 67, 220, "3 PLANKS TOP / 2 STICKS DOWN / = PICKAXE",
                  "south", wall=True)
        # V7: the tool tiers ahead (a sample of every tier: stop 7 smithy)
        self.sign(205, 67, 223, "UPGRADE LATER: / STONE, IRON, / DIAMOND "
                  "TOOLS", "east", wall=True)
        self.chest(205, 66, 224, [(0, PLANKS, 12), (1, STICK, 8),
                                  (2, WOOD_PICK, 1), (3, WOOD_AXE, 1),
                                  (4, WOOD_SHOVEL, 1), (5, WOOD_SWORD, 1)])
        # SE nook. V6b: 2x2 (x 208..209, z 223..224) so the door opens onto
        # a free cell beside the bed, not onto the bed foot.
        for y in (66, 67, 68):
            w.set(208, y, 222, PLANKS)
            w.set(209, y, 222, PLANKS)
            w.set(207, y, 223, PLANKS)
        w.set(207, 68, 224, PLANKS)
        w.door(207, 66, 224, "east")
        w.bed(209, 66, 224, "north")
        w.torch(209, 67, 223, "west")
        w.torch(205, 68, 221, "east")
        w.torch(206, 68, 224, "north")
        # Gate: cobble wall across the z=222 leg at x=217.
        for z in (221, 222, 223):
            for y in (66, 67):
                w.set(217, y, z, COBBLE)
        self.post_sign(217, 66, 220, "DIG THROUGH / USE THE PICK / KEEP THE "
                       "STONE", "west")
        self.reserved.add((217, 220))

    # ------------------------------------------------------- 2.5 stop 5 ---
    def quarry_carve(self):
        """Carve the quarry (before the roads, so L4 lies on its floor)."""
        w = self.w
        self.zones["quarry"] = (222, 216, 238, 222)
        for x in range(222, 239):
            for z in range(216, 223):
                for y in range(66, 80):
                    w.set(x, y, z, 0)
                w.set(x, 65, z, 1)
                self.walk.add((x, 66, z))

    def stop5_mine(self):
        w = self.w
        # Band face at z=215 (x 222..238): y66..71 stone + bands, 72 dirt, 73 grass.
        for x in range(222, 239):
            for y in range(66, 72):
                w.set(x, y, 215, 1)
            w.set(x, 72, 215, 3)
            w.set(x, 73, 215, 2)
        coal = [(223, 70), (225, 71), (227, 70), (229, 71), (233, 70),
                (234, 71), (235, 70), (237, 71), (238, 70)]
        iron = [(223, 69), (226, 68), (228, 69), (233, 69), (236, 68),
                (238, 69)]
        for (x, y) in coal:
            w.set(x, y, 215, 16)
        for (x, y) in iron:
            w.set(x, y, 215, 15)
        for (x0) in (226, 235):
            for dx in (0, 1):
                for y in (66, 67):
                    w.set(x0 + dx, y, 215, GRAVEL)
        self.sign(224, 70, 216, "COAL: ANY DEPTH", "south", wall=True)
        self.sign(224, 68, 216, "IRON: UNDER 64", "south", wall=True)
        self.sign(224, 66, 216, "GRAVEL FALLS", "south", wall=True)
        for x in (223, 229, 237):        # V6: (232, *) is the wider adit
            w.torch(x, 67, 216, "south")
        # Entrance tunnel into the face (x 230..231, z 213..215).
        for x in (230, 231):
            for z in (213, 214, 215):
                w.set(x, 65, z, COBBLE)
                for y in (66, 67, 68):
                    w.set(x, y, z, 0)
                self.walk.add((x, 66, z))
        self.mine_gallery()
        self.mine_spiral()
        self.mine_ladder()
        # Hut 7x5 (cobble + mossy, log corners, gravel floor, stone stairs).
        self.shed("s5_hut", 227, 227, 233, 231, 65, wall=(COBBLE, 0),
                  roof="wood", floor=(GRAVEL, 0), doors=[(230, 227), (233, 229)],
                  mossy=True, upper=(PLANKS, 0), chimney=(228, 228))
        for y in (66, 67):
            w.set(233, y, 229, 1)                 # stone plug in the east doorway
        self.sign(232, 67, 228, "MINE THE STONE / STONE NEEDS A / PICKAXE",
                  "west", wall=True)
        self.beacon(226, 226, "east", "MINE THE HILL / COAL IS BLACK / "
                    "IRON NEEDS A / STONE PICKAXE")
        self.chest(228, 66, 228, [(0, STONE_PICK, 1), (1, TORCH, 8),
                                  (2, COBBLE, 6), (3, COAL, 3), (4, 15, 3)])
        # Nook (228..229, 230). V6b: an open bed alcove (no door: the nook
        # is the bed, a door at (230,230) opened onto the bed foot).
        for x in (228, 229):
            for y in (66, 67, 68):
                w.set(x, y, 229, COBBLE)
        w.bed(229, 66, 230, "west")
        w.torch(228, 67, 230, "east")
        w.torch(231, 68, 228, "south")
        self.walk.add((233, 66, 229))
        self.walk.discard((233, 66, 229))

    def mine_ring(self):
        """32 slices of the 2-wide spiral lane, in walking order (2.5)."""
        sl = []
        for x in (231, 230, 229):
            sl.append(("S", x))
        for x in (228, 227):
            sl.append(("SWc", x))
        for z in range(210, 204, -1):
            sl.append(("W", z))
        for z in (204, 203):
            sl.append(("NWc", z))
        for x in range(229, 235):
            sl.append(("N", x))
        for x in (235, 236):
            sl.append(("NEc", x))
        for z in range(205, 211):
            sl.append(("E", z))
        for z in (211, 212):
            sl.append(("SEc", z))
        for x in (234, 233, 232):
            sl.append(("S", x))
        assert len(sl) == 32
        return sl

    @staticmethod
    def slice_cells(kind, c):
        if kind in ("S", "SWc"):
            return [(c, 211), (c, 212)]
        if kind in ("W", "NWc"):
            return [(227, c), (228, c)]
        if kind in ("N", "NEc"):
            return [(c, 203), (c, 204)]
        return [(235, c), (236, c)]          # E, SEc

    @staticmethod
    def slice_torch(kind, c):
        """(torch cell xz, facing) on the inner core wall, or None."""
        if kind == "S" and 229 <= c <= 234:
            return (c, 211), "south"
        if kind == "W":
            return (228, c), "west"
        if kind == "N":
            return (c, 204), "north"
        if kind == "E":
            return (235, c), "east"
        return None

    def mine_spiral(self):
        w = self.w
        ring = self.mine_ring()
        steps = []
        for i in range(108):                       # y 65 -> 12
            kind, c = ring[i % 32]
            y = 65 - i // 2
            steps.append((i, kind, c, y))
            for (x, z) in self.slice_cells(kind, c):
                w.set(x, y, z, COBBLE)
                for yy in (y + 1, y + 2, y + 3):
                    w.set(x, yy, z, 0)
                self.walk.add((x, y + 1, z))
        last_torch = -99
        for (i, kind, c, y) in steps:
            if i - last_torch < 4:
                continue
            t = self.slice_torch(kind, c)
            if t is None:
                continue
            (tx, tz), facing = t
            dx, dz = {"south": (0, -1), "north": (0, 1), "west": (1, 0),
                      "east": (-1, 0)}[facing]
            if not solid(self.bid(tx + dx, y + 2, tz + dz)):
                continue
            w.torch(tx, y + 2, tz, facing)
            last_torch = i
        # Second iron band on the north lane's outer wall (slice i=50, y=40).
        for x in (233, 234, 235):
            for y in (41, 42):
                w.set(x, y, 202, 15)
        self.sign(234, 42, 203, "IRON: UNDER 64", "south", wall=True)

    def mine_gallery(self):
        w = self.w
        for x in range(226, 235):
            for z in range(203, 208):
                w.set(x, 12, z, COBBLE)
                for y in (13, 14, 15):
                    w.set(x, y, z, 0)
                self.walk.add((x, 13, z))
        for z in (203, 204):
            for y in (13, 14):
                w.set(225, y, z, 14)               # gold
                w.set(235, y, z, 73)               # redstone
        for z in (206, 207):
            for y in (13, 14):
                w.set(225, y, z, 21)               # lapis
        for (y, z) in ((13, 206), (14, 206), (13, 207)):
            w.set(235, y, z, 56)                   # diamond x3
        self.sign(226, 15, 204, "GOLD: UNDER 32", "east", wall=True)
        self.sign(226, 15, 206, "LAPIS: UNDER 32", "east", wall=True)
        self.sign(234, 15, 204, "REDSTONE <16", "west", wall=True)
        self.sign(234, 15, 206, "DIAMOND <16", "west", wall=True)
        self.sign(230, 14, 203, "IRON PICK FOR / GOLD, REDSTONE / AND DIAMOND",
                  "south", wall=True)
        w.torch(226, 14, 205, "east")
        w.torch(234, 14, 205, "west")
        w.torch(228, 14, 203, "south")
        w.torch(230, 14, 207, "north")

    def mine_ladder(self):
        w = self.w
        top = self.ground(232, 207)
        for y in range(13, top + 1):
            w.ladder(232, y, 207, "north")
        self.sign(231, 15, 207, "CLIMB THE / LADDER UP", "north", wall=True)

    # ------------------------------------------------------- 2.6 stop 6 ---
    def stop6_torches(self):
        w = self.w
        self.shed("s6_shed", 254, 198, 258, 202, 65, doors=[(256, 202)],
                  lower=(COBBLE, 0), post="spruce", upper=(PLANKS, 0),
                  chimney=(258, 200), chimney_block=48)
        self.beacon(256, 204, "south", "CRAFT TORCHES / COAL ON STICK / "
                    "LIGHT THE WAY / AS YOU GO")
        # V6b: no partition (it split the 3x3 room into two 1-wide slots);
        # the door hangs in the doorway itself.
        w.door(256, 66, 202, "north")
        w.bed(255, 66, 200, "north")
        w.torch(255, 67, 199, "east")
        self.chest(257, 66, 199, [(0, TORCH, 8), (1, COAL, 4), (2, STICK, 4)])
        w.set(257, 66, 200, 58)
        w.torch(257, 67, 201, "west")
        # Tunnel: shell cobble x260..276, y65..69, z204..208 (deviation).
        self.footprints["tunnel"] = (260, 204, 276, 208)
        for x in range(260, 277):
            for z in range(204, 209):
                for y in range(65, 70):
                    edge = z in (204, 208) or y in (65, 69)
                    if edge:
                        mossy = (H(x, z, y) % 7 == 0) and z in (204, 208)
                        w.set(x, y, z, 48 if mossy else COBBLE)
                    else:
                        w.set(x, y, z, 0)
                self.reserved.add((x, z))
        for x in range(260, 277):
            for z in (205, 206, 207):
                w.slab(x, 65, z, "stone")
                for y in (66, 67, 68):
                    w.set(x, y, z, 0)
        for x in (266, 272):
            for z in (205, 206, 207):
                w.set(x, 69, z, 20)
        # V6b: a turf roof (cut-and-cover) over the shell, skylights left
        # open, so its top reads as hillside, not a bare cobble platform
        for x in range(260, 277):
            for z in range(204, 209):
                if self.bid(x, 69, z) != 20:
                    w.set(x, 70, z, 2)
        self.sign(261, 67, 205, "MONSTERS SPAWN / IN THE DARK / LIGHT 8 OR "
                  "MORE / KEEPS THEM OUT", "south", wall=True)
        # Porch outside the east mouth, lit on 4 fence posts.
        self.zones["porch"] = (277, 204, 281, 208)
        for x in range(277, 282):
            for z in range(204, 209):
                w.set(x, 65, z, COBBLE)
                for y in range(66, 70):
                    w.set(x, y, z, 0)
                self.walk.add((x, 66, z))
                self.reserved.add((x, z))
        for (x, z) in ((278, 204), (281, 208)):      # V6: 2 of the 4
            self.lamp(x, z, 65)
            self.walk.discard((x, 66, z))
        self.sign(277, 67, 204, "LIT: NO SPAWNS", "east", wall=True)

    # ------------------------------------------------------- 2.7 stop 7 ---
    def stop7_smelting(self):
        w = self.w
        top = self.shed("s7_shed", 284, 205, 292, 211, 65, wall=(COBBLE, 0),
                        lower=(45, 0), doors=[(288, 211)], trim="orange",
                        mid_posts=4)
        w.set(288, top + 1, 208, 43)
        w.set(288, top + 2, 208, 43)
        w.set(288, top + 3, 208, SLAB)
        w.set(288, 65, 212, GRAVEL)
        self.walk.add((288, 66, 212))
        self.beacon(290, 216, "north", "SMELT IN A / FURNACE / ORE ON TOP / "
                    "FUEL BELOW")
        w.furnace(286, 66, 206, "south", lit=True,
                  items=[(0, 15, 4), (1, COAL, 2), (2, IRON_INGOT, 2)],
                  burn_time=1200, cook_time=100)
        w.furnace(290, 66, 206, "south")
        self.sign(286, 67, 206, "LEFT: SMELTING / RIGHT: YOURS / FILL TOP + "
                  "FUEL", "south", wall=True)
        self.sign(288, 67, 206, "BUILD A FURNACE / 8 COBBLESTONE / BURN COAL / "
                  "SMELT THE ORE", "south", wall=True)
        # V6b: the chest fills the 1x1 pocket beside the empty furnace (the
        # 'yours' one), with what it takes to fill it
        self.chest(291, 66, 206, [(0, COAL, 8), (1, 15, 6), (2, COBBLE, 8),
                                  (3, 12, 8), (4, 77, 1)])
        # V6b: a crafting table, so 'build a furnace' can be done here.
        w.set(285, 66, 210, 58)
        # V7: charcoal (FurnaceRecipes: log -> coal damage 1)
        self.sign(285, 67, 207, "SMELT A LOG / FOR CHARCOAL / WHEN YOU HAVE / "
                  "NO COAL", "east", wall=True)
        # East-end bed alcove: interior (291, 208..209). V6b: open (a door
        # at (290,209) opened onto the bed foot).
        for y in (66, 67, 68):
            w.set(290, y, 208, COBBLE)
            w.set(291, y, 207, COBBLE)
        w.bed(291, 66, 209, "north")
        w.torch(291, 67, 208, "west")
        w.torch(285, 68, 208, "east")
        w.torch(288, 68, 210, "north")
        # Gate: cobble wall x=296, z213..215 with a closed iron door.
        for z in (213, 215):
            for y in (66, 67):
                w.set(296, y, z, COBBLE)
        w.door(296, 66, 214, "west", material="iron")
        self.sign(295, 67, 213, "SMELT COBBLE / TO STONE, MAKE / A BUTTON, "
                  "OPEN / THE IRON DOOR", "west", wall=True)

    # ------------------------------------------------------- 2.8 stop 8 ---
    def stop8_farm(self):
        """V6 farm: the yard SHOWS farming. A 9x9 tilled field with a central
        water channel and wheat at mixed stages fills the north-west; the
        teaching rows stay (growth stages 0..7 with their slab kerb, sugar
        cane on the rim of a small pond); a fence pen with a double-slab
        trough; pumpkins by the south fence; a barn with a cobble base,
        spruce frame and plank infill (red wool on the door only)."""
        w = self.w
        self.zones["farm"] = (304, 232, 322, 250)
        x1, z1, x2, z2 = 304, 232, 322, 250
        # V6: the east gap is fenced (it led nowhere; the pen stands there)
        gaps = {(304, z) for z in (239, 240, 241)} | \
               {(x, 250) for x in (313, 314, 315)}
        for x in range(x1, x2 + 1):
            for z in range(z1, z2 + 1):
                self.reserved.add((x, z))
                edge = x in (x1, x2) or z in (z1, z2)
                if not edge:
                    if self.bid(x, 65, z) == 2:
                        self.walk.add((x, 66, z))
                    continue
                if (x, z) in gaps:
                    continue
                corner = x in (x1, x2) and z in (z1, z2)
                if corner:
                    for y in (66, 67):
                        w.log(x, y, z, "spruce")
                else:
                    w.set(x, 66, z, FENCE)
        # V6: jack-o'-lanterns only on the two entrance-side (west) corners.
        for (x, z, f) in ((304, 232, "west"), (304, 250, "west")):
            w.pumpkin(x, 68, z, f, lit=True)
        self.beacon(305, 242, "east", "HOE THE GRASS / PLANT SEEDS / WATER "
                    "WITHIN 4 / GIVE IT LIGHT")
        # Barn (spruce frame, plank infill, cobble base), NE corner.
        self.shed("s8_shed", 316, 232, 322, 236, 65, wall=(PLANKS, 0),
                  post="spruce", doors=[(319, 236)], trim="red",
                  lower=(COBBLE, 0), upper=(PLANKS, 0), mid_posts=3)
        self.chest(317, 66, 233, [(0, WOOD_HOE, 1), (1, SEEDS, 8),
                                  (2, BONE_MEAL, 4, 15), (3, BUCKET, 1),
                                  (4, WHEAT, 3), (5, BREAD, 1)])
        for y in (66, 67, 68):
            w.set(320, y, 233, PLANKS)
            w.set(320, y, 234, PLANKS)
        w.door(320, 66, 235, "east")
        w.set(320, 68, 235, PLANKS)
        w.bed(321, 66, 234, "north")
        w.torch(321, 67, 233, "south")
        w.torch(318, 68, 233, "south")
        # 9x9 field x 306..314, z 233..241: farmland, wheat at mixed stages,
        # a 1-wide water channel down x = 310 (every cell within 4 of it).
        for x in range(306, 315):
            for z in range(233, 242):
                if x == 310:
                    w.water_source(x, 65, z)
                    w.set(x, 64, z, 3)
                else:
                    h = H(x, z, 61) % 10
                    stage = 7 if h < 5 else (2 + h % 5)
                    w.crop(x, 66, z, stage, 7)
                self.walk.discard((x, 66, z))
        self.post_sign(305, 66, 233, "A REAL FIELD: / WATER IN THE / "
                       "MIDDLE, WHEAT / ALL ROUND", "south")
        self.walk.discard((305, 66, 233))
        # Teaching rows: growth stages 0..7 with the slab kerb.
        for k in range(8):
            x = 306 + k
            w.crop(x, 66, 244, k, 7)
            self.walk.discard((x, 66, 244))
            self.growth.add((x, 67, 244))
            w.slab(x, 66, 243, "stone")
            self.walk.discard((x, 66, 243))
            for z in (245, 246):                   # the pond
                w.set(x, 65, z, 9)
                w.set(x, 64, z, 3)
                self.walk.discard((x, 66, z))
        self.post_sign(305, 66, 244, "STAGE 0 TO 7 / PICK AT 7 / BONE MEAL / "
                       "SKIPS TO 7", "west")
        self.walk.discard((305, 66, 244))
        self.lamp(309, 243, 65)
        self.lamp(314, 244)
        self.walk.discard((314, 66, 244))
        # Sugar cane on the pond's south rim.
        for x in range(306, 311):
            w.reed(x, 66, 247, 3)
            self.walk.discard((x, 66, 247))
        self.post_sign(312, 66, 247, "CANE GROWS BY / WATER, UP TO 3 / CUT THE "
                       "CANE / TO PASS", "south")
        self.walk.discard((312, 66, 247))
        # Practice: a bucket hole by the south gate path.
        w.set(315, 65, 248, 0)
        self.walk.discard((315, 66, 248))
        self.walk.add((315, 65, 248))
        self.post_sign(316, 66, 248, "DIG A HOLE / POUR WATER", "west")
        self.walk.discard((316, 66, 248))
        # Fence pen, fence ring x 316..321, z 238..246 (V6c: grown south
        # from z 242 so 8 animals have room); grass floor x 317..320,
        # z 239..245 with a sunk water trough along its north side.
        px1, pz1, px2, pz2 = PEN_BOX
        for x in range(px1, px2 + 1):
            for z in range(pz1, pz2 + 1):
                if x in (px1, px2) or z in (pz1, pz2):
                    if (x, z) != (px1, 240):
                        w.set(x, 66, z, FENCE)
                        self.walk.discard((x, 66, z))
                else:
                    w.set(x, 65, z, 2)             # grass floor
        for (x, z) in ((px1, pz1), (px2, pz1), (px1, pz2), (px2, pz2),
                       (px1, 242), (px2, 242),
                       (308, 250), (319, 250), (306, 232), (313, 232)):
            w.torch(x, 67, z, "floor")             # torches on fence posts
        # V6b: Beta 1.7.3 has no fence gate, so the pen's way in is a
        # wooden door (mobs never open doors in 1.7.3): the pen holds
        # animals once the player brings some in.
        w.door(px1, 66, 240, "east")
        self.walk.discard((px1, 66, 240))
        # V6c: a water trough sunk flush in the grass (a source in a dirt
        # cup cannot flow: every side neighbour at y 65 is solid).
        for x in (318, 319, 320):
            w.water_source(x, 65, 239)
            w.set(x, 64, 239, 3)
            self.walk.discard((x, 66, 239))
        self.post_sign(317, 66, 241, "A PEN: THE / DOOR KEEPS / THE "
                       "ANIMALS IN", "west")
        self.walk.discard((317, 66, 241))
        # Pumpkins along the south fence.
        for x in (318, 319, 320):
            w.pumpkin(x, 66, 249, "north")
            self.walk.discard((x, 66, 249))
        self.post_sign(319, 66, 247, "PUMPKINS GROW / WILD ONLY / PUMPKIN + "
                       "TORCH / = LANTERN", "north")
        self.walk.discard((319, 66, 247))
        # South gate: reeds on 313 and 315 around a water source at 314.
        for x in (313, 315):
            w.set(x, 65, 250, 2)
            w.reed(x, 66, 250, 2)
        w.water_source(314, 65, 250)
        w.set(314, 64, 250, 3)
        # The yard is lit by glowstone set flush in the grass walks; the
        # light pass tops up any cell the lattice misses.
        self.floor_glow(305, 233, 321, 249, 65, step=8,
                        avoid=(59, 60, 83, 8, 9, 86, 54, 58, 26, 63, 64, 85))


    # ------------------------------------------------------- 2.9 stop 9 ---
    def stop9_cottage(self):
        w = self.w
        self.shed("s9_cottage", 310, 269, 318, 275, 65, checker=True,
                  doors=[(314, 269), (314, 275)], trim="red",
                  lower=(COBBLE, 0), upper=(35, 0), chimney=(311, 272))
        w.door(314, 66, 275, "north")
        self.beacon(308, 268, "east", "BUILD A SHELTER / PLACE THE DOOR / "
                    "LIGHT INSIDE / SLEEP AT NIGHT")
        self.sign(315, 67, 270, "A DOOR IS / 6 PLANKS", "south", wall=True)
        w.set(317, 66, 270, 58)
        w.furnace(317, 66, 271, "west")
        self.chest(311, 66, 274, [(0, WOOD_DOOR_ITEM, 1), (1, PLANKS, 24),
                                  (2, TORCH, 4), (3, 65, 2)])
        # West-room nook (311..312, 270..272), door (313,271).
        for z in (270, 272):
            for y in (66, 67, 68):
                w.set(313, y, z, PLANKS)
        w.set(313, 68, 271, PLANKS)
        for x in (311, 312):
            for y in (66, 67, 68):
                w.set(x, y, 273, PLANKS)
        w.door(313, 66, 271, "west")
        w.bed(311, 66, 271, "north")
        w.torch(312, 67, 272, "north")
        w.torch(317, 68, 273, "west")
        w.torch(311, 68, 274, "east")
        # Build pad 5x5 kerbed.
        self.zones["buildpad"] = (319, 269, 325, 275)
        for x in range(319, 326):
            for z in range(269, 276):
                self.reserved.add((x, z))
                if x in (319, 325) or z in (269, 275):
                    w.slab(x, 66, z, "stone")
                else:
                    self.walk.add((x, 66, z))
        for (x, z) in ((322, 269), (322, 275)):      # V6: 2 of the 4
            self.lamp(x, z, 65)
        self.post_sign(322, 66, 277, "BUILD YOUR OWN / HUT HERE", "south")
        self.reserved.add((322, 277))

    # ------------------------------------------------------ 2.10 stop 10 ---
    def stop10_bedhouse(self):
        w = self.w
        self.shed("s10_house", 293, 295, 299, 301, 65, doors=[(296, 295),
                  (293, 298)], trim="red", lower=(COBBLE, 0),
                  upper=(45, 0), chimney=(298, 300))
        self.beacon(300, 294, "north", "LAST STOP / CRAFT A BED / THEN THE "
                    "HUB")
        w.bed(294, 66, 297, "north")
        self.sign(295, 67, 296, "CRAFT A BED / 3 WOOL 3 PLANKS / SLEEP WHEN "
                  "DARK / SKIP THE NIGHT", "south", wall=True)
        self.sign(297, 67, 300, "PLACE YOUR BED", "north", wall=True)
        # V6b: the bed recipe is 3x3, so the house needs a crafting table
        # (SE corner; (295..297, 66, 300) stays free as the bed spot).
        w.set(298, 66, 300, 58)
        self.sign(298, 67, 300, "TABLE: CRAFT / THE BED HERE", "north",
                  wall=True)
        self.chest(298, 66, 296, [(0, BED_ITEM, 1), (1, 35, 3), (2, PLANKS, 3),
                                  (3, BREAD, 1), (4, BREAD, 1)])
        w.torch(298, 68, 298, "west")
        w.torch(294, 68, 300, "north")
        # V7: a bed is a respawn point, not only a night skip (sleeping
        # sets the spawn, EntityPlayer.sleepInBedAt; monsters near refuse
        # the sleep)
        self.sign(294, 67, 300, "SLEEP TO SET / YOUR SPAWN: / DIE AND YOU / "
                  "COME BACK HERE", "east", wall=True)
        self.sign(298, 67, 298, "KEEP MONSTERS / AWAY: NEAR / ONES STOP / "
                  "YOUR SLEEP", "west", wall=True)
        # Hub gate: wall x=288 z297..299, iron door, button on the east side.
        for z in (297, 299):
            for y in (66, 67):
                w.set(288, y, z, COBBLE)
        w.door(288, 66, 298, "west", material="iron")
        w.button(289, 67, 297, "east")
        self.sign(289, 67, 299, "PRESS THE / BUTTON TO OPEN / THE HUB IS NEXT",
                  "east", wall=True)

    # ---------------------------------------------------------- 2.11 hub ---
    def hub(self):
        w = self.w
        self.zones["plaza"] = (244, 244, 268, 268)
        entrances = {(x, 244) for x in (255, 256, 257)} | \
                    {(x, 268) for x in (255, 256, 257)} | \
                    {(268, z) for z in (255, 256, 257)} | \
                    {(244, z) for z in (255, 256, 257)} | \
                    {(x, 268) for x in (244, 245, 246)}
        for x in range(244, 269):
            for z in range(244, 269):
                w.set(x, 65, z, 1)
                for y in range(66, 72):
                    w.set(x, y, z, 0)
                self.reserved.add((x, z))
                edge = x in (244, 268) or z in (244, 268)
                if edge and (x, z) not in entrances:
                    w.slab(x, 66, z, "stone")
                else:
                    self.walk.add((x, 66, z))
        for (x, z) in ((256, 243), (255, 243), (257, 243),
                       (255, 269), (256, 269), (257, 269)):
            w.set(x, 65, z, 1)
            self.walk.add((x, 66, z))
            self.reserved.add((x, z))
        # Dais + beacon + 4 signs.
        for x in (255, 256, 257):
            for z in (255, 256, 257):
                w.set(x, 66, z, 24)
                self.walk.discard((x, 66, z))
                self.walk.add((x, 67, z))
        self.walk.discard((256, 67, 256))
        w.log(256, 67, 256)
        w.log(256, 68, 256)
        w.pumpkin(256, 69, 256, "south", lit=True)
        self.sign(257, 68, 256, "NEXT: 11 SOUTH / THEN 12, 13 / AND 14",
                  "east", wall=True)
        self.sign(256, 68, 255, "13 NORTH: / REDSTONE HALL", "north",
                  wall=True)
        self.sign(255, 68, 256, "12 WEST: / MINECART YARD", "west", wall=True)
        self.sign(256, 68, 257, "11 SOUTH: / MONSTER LAB", "south", wall=True)
        self.post_sign(247, 66, 267, "15 THE END: / DOWN THIS RAMP / WEST TO "
                       "THE / LIGHTHOUSE", "north")
        self.walk.discard((247, 66, 267))
        # V7: the trail arrives here; say what is basic and what is extra
        self.post_sign(249, 66, 267, "LEARN BASICS / AT 1 TO 10, / TRY "
                       "EXTRAS / AT 11 TO 15", "south")
        self.walk.discard((249, 66, 267))
        # V6: half the edge lanterns (the exit gates light the other sides).
        for (x, z) in ((244, 250), (244, 262), (268, 250), (268, 262)):
            self.lamp(x, z, 65)
        # V6: a sparse floor-glow lattice (step 9, was 6); the stall
        # canopies, the obelisks, the exit gates and the avenue arrow light
        # the rest.
        self.plaza_v2()
        self.floor_glow(245, 245, 267, 267, 65, step=9,
                        avoid=(9, 85, 92, 84, 25, 47, 58, 17, 86, 44))

    # V6 hub exits, numbered on from the trail (n, label, colour, centre,
    # approach side). 13 (the keep) carries its numeral on the gatehouse.
    HUB_EXITS = [
        (11, ["11 NEXT", "MONSTER LAB"], "purple", (256, 268), "north"),
        (12, ["12", "MINECARTS"], "black", (244, 256), "east"),
        (14, ["14", "PISTON BRIDGE"], "blue", (268, 256), "west"),
    ]

    # V8: which side of each hub exit its board stands on (1 = the
    # approaching player's right): 14 takes its north half so the boards of
    # 11 and 14 do not meet in the plaza's south-east corner
    HUB_BOARD_SIDE = {14: -1}

    def awning(self, cx, cz, y, facing, half, depth, col):
        """Striped wool awning (stop colour / white) at height y over an
        exit: `half` cells either side, `depth` rows from the gate line
        toward the approach."""
        dx, dz = wl.DIRS[facing]
        rx, rz = self.right_of(facing)
        for t in range(depth):
            for u in range(-half, half + 1):
                x, z = cx + rx * u + dx * t, cz + rz * u + dz * t
                self.w.wool(x, y, z, col if u % 2 == 0 else "white")

    def hub_exits(self):
        """V6: every plaza exit is a numbered gate (wool numeral in its room
        colour, facing into the plaza). The next one, 11, is emphasised:
        wider, glowstone columns for posts and a deeper awning; a stone
        arrow on the south avenue points at it. The 5-wide approach to the
        keep (x 254..258) stays clear. Runs after the hill lift (final
        coordinates), so each board's back layer may stand over a lower
        terrace."""
        w = self.w
        fy = 65 + self.hub_dy
        for (n, label, col, (cx, cz), facing) in self.HUB_EXITS:
            nxt = n == 11
            half = 3 if nxt else 2
            self.gate_frame(cx, cz, fy, facing, n, col, label, half=half,
                            post=("block", 89) if nxt else ("log", "spruce"),
                            torches=not nxt, label_on_post=True,
                            board_clear=(44, COBBLE, 89, TORCH),
                            board_side=self.HUB_BOARD_SIDE.get(n, 1))
            self.awning(cx, cz, fy + 4, facing, half, 3 if nxt else 2, col)
            if nxt:
                # V6b: exit 11 has no post torches and a 3-deep awning, so
                # its top gets floor torches (a dark awning over the hub
                # is a spawn platform)
                rx, rz = self.right_of(facing)
                dx, dz = wl.DIRS[facing]
                xs = [cx + rx * u + dx * t for u in (-half, half)
                      for t in (0, 2)]
                zs = [cz + rz * u + dz * t for u in (-half, half)
                      for t in (0, 2)]
                self.sealed.append((min(xs), min(zs), max(xs), max(zs),
                                    fy + 5, fy + 5, "torch"))
            # the lintel keeps the colour wool over each post
            rx, rz = self.right_of(facing)
            for k in (-half, half):
                w.wool(cx + rx * k, fy + 4, cz + rz * k, col)
        # stone arrow on the south avenue, pointing at exit 11
        cells = dict(self.arrow_cells((256, 264), (0, 1)))
        for x in range(254, 259):
            for z in range(262, 267):
                ch = cells.get((x, z))
                w.set(x, fy, z, 24 if ch is None else
                      (89 if ch == "G" else COBBLE))

    # ------------------------------------------------- 2.12 redstone hall ---
    def redstone_hall(self):
        w = self.w
        top = self.shed("hall", 246, 226, 266, 242, 65, wall=(COBBLE, 0),
                        wall_h=4, roof=None, floor=(COBBLE, 0),
                        doors=[(256, 242)])
        # V6b: the roof deck is lit by floor torches like the lab's (the
        # 'glow' deck set glowstone into the crenels and parapet walk)
        self.castle_keep(246, 226, 266, 242, 65, 4, deck="torch")
        w.set(256, 68, 242, 0)
        w.door(256, 66, 242, "north")
        for (x, y) in ((255, 66), (255, 67), (257, 66), (257, 67), (254, 66),
                       (254, 67), (258, 66), (258, 67), (255, 68), (256, 68),
                       (257, 68)):
            w.wool(x, y, 242, "red")
        for x in (248, 256, 264):
            w.torch(x, 68, 227, "south")
            w.torch(x, 68, 241, "north")
        # ceiling lights: V6b adds a north pair over the clock and the
        # ripple line (the 'glow' roof deck used to light this row)
        for x in (251, 261):
            for z in (229, 234):
                w.set(x, top, z, 89)
        self.clock_demo()
        self.ripple_demo()
        self.floor_glow(247, 227, 265, 241, 65, step=6,
                        avoid=(55, 93, 94, 29, 33, 34, 69, 75, 76, 77, 64))
        # V6b: the build pad the sign promises: a 7x5 planks pad in the
        # cobble floor (its lit corners are the floor lights at 253/259 x
        # 234/240), a crafting table beside it, the sign at its south edge.
        for x in range(253, 260):
            for z in range(235, 240):
                w.set(x, 65, z, PLANKS)
        w.set(252, 66, 237, 58)
        self.walk.discard((252, 66, 237))
        self.post_sign(256, 66, 240, "BUILD YOUR OWN / ON THIS PAD / CHEST "
                       "HAS PARTS", "north")
        self.walk.discard((256, 66, 240))
        self.chest(247, 66, 241, [(0, REDSTONE, 16), (1, REPEATER_ITEM, 4),
                                  (2, 69, 2), (3, 77, 2), (4, 76, 4),
                                  (5, COBBLE, 8), (6, 29, 2),
                                  (7, WOOD_DOOR_ITEM, 1)])
        self.walk.discard((247, 66, 241))

    def clock_demo(self):
        """3.1 rebuilt (see DEVIATIONS): T on B's north face; feed
        T -> W W W -> R4 R4 -> W; down x=254 to the return row z=230 ->
        west to (248,230) -> repeater (248,229) facing north into B.
        Half period = 8 + 8 + 2 (return repeater) + 2 (torch) = 20 game
        ticks; 5 toggles per 100 ticks < 8 (BlockRedstoneTorch.java:24-26).
        Nothing adjacent to B except T (does not power its own mount), the
        return repeater (powers B only through its output face,
        BlockRedstoneRepeater.isPoweringTo) and the START button."""
        w = self.w
        w.set(248, 66, 228, COBBLE)                          # B
        w.torch(248, 66, 227, "north", redstone=True, lit=True)  # T
        for x in (249, 250, 251):
            w.wire(x, 66, 227)
        w.repeater(252, 66, 227, "east", delay=4)
        w.repeater(253, 66, 227, "east", delay=4)
        for z in (227, 228, 229, 230):
            w.wire(254, 66, z)
        for x in range(248, 254):
            w.wire(x, 66, 230)
        w.repeater(248, 66, 229, "north", delay=1)
        w.button(247, 66, 228, "west")                       # START
        # Door branch: wire (251,231) -> repeater south -> door (251,233).
        w.wire(251, 66, 231)
        w.repeater(251, 66, 232, "south", delay=1)
        for x in (249, 250, 252, 253):
            for y in (66, 67, 68):
                w.set(x, y, 233, COBBLE)
        w.set(251, 68, 233, COBBLE)
        w.door(251, 66, 233, "north")
        self.sign(250, 68, 227, "WATCH THE CLOCK / TORCH + 2 X 4 / DOOR FLAPS / "
                  "BUTTON RESTARTS", "south", wall=True)

    def ripple_demo(self):
        """3.2 rebuilt (see DEVIATIONS): button (256,230) on the pillar's
        south face; W 257; repeaters east at 258/260/262/264 with delays
        1..4; W* wires at 259/261/263/265 power the cobble under them; that
        cobble is the SOUTH neighbour of a sticky piston (x,65,229) facing up
        (BlockPistonBase.java:79 first row), and the wire is Manhattan 2
        from the piston so its update reaches it (BlockRedstoneWire.java:
        175-181). Tiles pop at +2, +6, +12, +20 game ticks."""
        w = self.w
        w.set(256, 66, 229, COBBLE)
        w.button(256, 66, 230, "south")
        w.wire(257, 66, 230)
        colours = ("red", "orange", "yellow", "green")
        for k in range(4):
            rx = 258 + 2 * k
            w.repeater(rx, 66, 230, "east", delay=k + 1)
            px = rx + 1
            w.wire(px, 66, 230)
            w.piston(px, 65, 229, "up", sticky=True)
            w.wool(px, 66, 229, colours[k])
            self.walk.discard((px, 66, 229))
        self.sign(261, 68, 227, "PRESS BUTTON / WATCH THE WAVE / 1 2 3 4 TICKS",
                  "south", wall=True)

    # ---------------------------------------------------- 2.13 cart yard ---
    def cart_yard(self):
        w = self.w
        self.zones["yard"] = (206, 246, 240, 266)
        for x in range(206, 241):
            for z in range(246, 267):
                self.reserved.add((x, z))
                for y in range(66, 72):
                    w.set(x, y, z, 0)
                edge = x in (206, 240) or z in (246, 266)
                # V3: no gravel field. Stone platform along the rail
                # corridor (z 254..258), grass strips everywhere else.
                w.set(x, 65, z, 1 if 254 <= z <= 258 else 2)
                if edge:
                    if not (x == 240 and 255 <= z <= 257):
                        w.set(x, 66, z, FENCE)
                    else:
                        self.walk.add((x, 66, z))
                else:
                    self.walk.add((x, 66, z))
        # Track z=256: bumper 207, powered 208, plain 209, detector 210,
        # plain 211..235 with boosters at 216/224/232, detector 236, plain 237,
        # powered 238, bumper 239.
        for x in range(207, 240):
            w.set(x, 65, 256, PLANKS)
        for x in (207, 239):
            w.set(x, 66, 256, COBBLE)
        for x in range(208, 239):
            if x in (208, 238):
                w.powered_rail(x, 66, 256, "ew")
            elif x in (210, 236):
                w.powered_rail(x, 66, 256, "ew", detector=True)
            elif x in (216, 224, 232):
                w.powered_rail(x, 66, 256, "ew", powered=True)
                w.set(x, 65, 257, PLANKS)
                w.torch(x, 66, 257, "floor", redstone=True, lit=True)
                self.walk.discard((x, 66, 257))
            else:
                w.rail(x, 66, 256, "ew")
            self.walk.discard((x, 66, 256))
        self.walk.discard((207, 66, 256))
        self.walk.discard((239, 66, 256))
        w.button(207, 66, 257, "south")
        w.button(239, 66, 257, "south")
        for x in (210, 236):
            # the arrival bell keeps its V2 sound: the instrument comes from
            # the material under the note block (TileEntityNote.java:32-46),
            # so it stays on gravel (sand material) under the V3 stone floor
            w.set(x, 65, 255, GRAVEL)
            w.noteblock(x, 66, 255, 12)
            self.walk.discard((x, 66, 255))
        self.post_sign(216, 66, 258, "A TORCH POWERS / GOLD RAILS", "south")
        self.walk.discard((216, 66, 258))
        # Shelters over each station: 4 log posts + a roof.
        for (xa, xb) in ((206, 210), (236, 240)):
            for x in (xa, xb):
                for z in (254, 258):
                    for y in (66, 67, 68):
                        w.log(x, y, z)
                    self.walk.discard((x, 66, z))
            if xa == 206:
                self.gable(xa, 254, xb, 258, 69, "x", "wood")
            else:
                # V6: the east station gets a flat slab canopy so the view
                # west from the plaza's west gate (exit 12) to the CAVEX
                # sign over the sea stays open
                for x in range(xa - 1, xb + 1):
                    for z in range(253, 260):
                        w.slab(x, 69, z, "wood")
        self.practice_bays()
        self.yard_lights()
        self.chest(207, 66, 255, [(0, MINECART, 1), (1, 66, 8), (2, 27, 4),
                                  (3, 76, 2), (4, 25, 1),
                                  (5, MINECART, 1)])
        self.walk.discard((207, 66, 255))
        self.sign(207, 67, 254, "RIDE THE CART / PRESS BUTTON / TO LAUNCH / "
                  "BELL = ARRIVAL", "east", wall=True)
        self.sign(207, 67, 258, "PLACE A CART / ON A GOLD RAIL / THEN PRESS",
                  "east", wall=True)
        self.sign(239, 67, 254, "GOLD RAILS PUSH / PLAIN ONES / ROLL", "west",
                  wall=True)
        # Practice siding z=262, a cobble bumper at each end (V6b: the east
        # end was open). V6b: the loose log 'stacks' on the south strip are
        # gone (scattered filler).
        for x in (211, 218):
            w.set(x, 66, 262, COBBLE)
            self.walk.discard((x, 66, 262))
        for x in range(212, 218):
            w.set(x, 65, 262, PLANKS)
            w.rail(x, 66, 262, "ew")
            self.walk.discard((x, 66, 262))

    def yard_lights(self):
        """V6b cart-yard lighting (replaces the 25-glowstone lawn lattice):
        torches on the north yard fence every 4 blocks, cobble lantern
        pillars in the south fence every 4 (the south strip is 7 deep with
        nothing else to carry a light), cobble lanterns along the two
        platform edges (z 254 and 258) between the stations, and a wall
        torch on each shelter post's platform face. Pre-lift coordinates
        (the yard is lifted +3 later)."""
        w = self.w
        for x in range(210, 239, 4):
            w.torch(x, 67, 246, "floor")            # torch on a fence post
            self.lamp(x, 266, 65)                   # lantern pillar
        for (x, z) in ((214, 254), (222, 254), (230, 254),
                       (219, 258), (227, 258), (234, 258)):
            self.lamp(x, z, 65)
            self.walk.discard((x, 66, z))
        for (x, f) in ((210, "east"), (236, "west")):
            for z in (254, 258):
                w.torch(x + wl.DIRS[f][0], 68, z, f)
        # a torch on the outer (north / south) face of every shelter post
        for x in (206, 210, 236, 240):
            w.torch(x, 67, 253, "north")
            w.torch(x, 67, 259, "south")

    def practice_bays(self):
        """V6 practice ground (cart yard, north grass strip, pre-lift y 65):
        three bays split by low log posts, each with its instruction sign
        right beside its materials: BREAK (a pile of logs, dirt and cobble),
        PLACE (a double-slab outline to fill, chest of blocks beside it) and
        JUMP (a 2-wide, 1-deep gap to run and jump; fall in and you just
        walk out). South of the rail a plank boarding platform marks where to
        get on; the view from the trail (L2) to the line is kept clear."""
        w = self.w
        # V6b: the bays are split by fence rows joined to the yard fence
        # (the V6 single logs read as scattered blocks), torches on top
        for x in (218, 229):
            for z in range(247, 254):
                w.set(x, 66, z, FENCE)
                self.walk.discard((x, 66, z))
            for z in (247, 251):
                w.torch(x, 67, z, "floor")
        for (x, y, z, b) in ((208, 66, 251, 17), (209, 66, 251, 17),
                             (208, 67, 251, 17), (210, 66, 251, 3),
                             (208, 66, 252, COBBLE), (209, 66, 252, 3)):
            w.set(x, y, z, b)
        self.post_sign(211, 66, 252, "BREAK: AIM AT / A BLOCK AND / HOLD THE "
                       "BREAK / BUTTON", "south")
        for x in range(221, 226):
            for z in range(248, 253):
                # V6b: plain double-slab corners (the glowstone corners were
                # part of the lawn lattice; the fence torches light the bay)
                if x in (221, 225) or z in (248, 252):
                    w.slab(x, 65, z, "stone", double=True)
        # V6b: the chest stands at the outline's corner beside its sign
        self.chest(226, 66, 252, [(0, COBBLE, 16), (1, PLANKS, 16), (2, 3, 16)])
        self.post_sign(226, 66, 253, "PLACE: FILL / THE SLAB / OUTLINE WITH / "
                       "BLOCKS", "south")
        for x in (234, 235):
            for z in range(247, 254):
                w.set(x, 65, z, 0)
                self.walk.discard((x, 66, z))
                self.walk.add((x, 65, z))
        self.post_sign(232, 66, 253, "JUMP: RUN AT / THE GAP AND / JUMP "
                       "ACROSS", "south")
        for x in range(208, 215):
            for z in (257, 258):
                w.set(x, 65, z, PLANKS)
        self.post_sign(213, 66, 258, "BOARD HERE: / CART ON THE / RAIL, THEN "
                       "/ GET IN", "east")
        for p in ((211, 66, 252), (226, 66, 253), (232, 66, 253),
                  (213, 66, 258)):
            self.walk.discard(p)
        self.soft["yard_view"] = (196, 249, 205, 263)

    # -------------------------------------------------- 2.14 monster lab ---
    def monster_lab(self):
        w = self.w
        top = self.shed("lab", 248, 270, 264, 284, 65, wall=(COBBLE, 0),
                        wall_h=4, roof=None, floor=(COBBLE, 0),
                        doors=[(256, 270)])
        # V2: the lab is the castle's south tower block (no crest, two
        # turrets); its roof deck is lit by torches only, so no light
        # reaches the dark cell through the corridor (see build()).
        self.castle_keep(248, 270, 264, 284, 65, 4, crest=False,
                         turrets="north", deck="torch")
        w.set(256, 68, 270, 0)
        w.door(256, 66, 270, "south")
        for (x, y) in ((255, 66), (255, 67), (257, 66), (257, 67), (254, 66),
                       (254, 67), (258, 66), (258, 67), (255, 68), (256, 68),
                       (257, 68)):
            w.wool(x, y, 270, "purple")
        for (xa, xb, gx) in ((249, 253, 254), (259, 263, 258)):
            for x in range(min(xa, gx), max(xb, gx) + 1):
                for z in range(276, 283):
                    for y in (66, 67, 68):
                        edge = z in (276, 282) or x == gx
                        if edge:
                            w.set(x, y, z, 20 if (x == gx and 277 <= z <= 281)
                                  else COBBLE)
                        else:
                            w.set(x, y, z, 0)
                    w.set(x, 69, z, COBBLE)
                    self.walk.discard((x, 66, z))
            for x in range(xa, xb + 1):
                for y in (66, 67, 68):
                    w.set(x, y, 283, COBBLE)
                self.walk.discard((x, 66, 283))
        self.spawner(251, 66, 279)
        self.spawner(261, 66, 279)
        for (x, z, f) in ((260, 277, "south"), (262, 277, "south"),
                          (260, 281, "north"), (262, 281, "north")):
            w.torch(x, 67, z, f)
        self.sign(254, 67, 275, "DARK CELL / MONSTERS SPAWN", "north", wall=True)
        self.sign(258, 67, 275, "LIT CELL / NOTHING SPAWNS", "north", wall=True)
        self.sign(256, 67, 283, "THEY CANNOT / GET OUT", "north", wall=True)
        for x in (252, 260):
            w.torch(x, 68, 271, "south")
        w.torch(249, 68, 273, "east")
        w.torch(263, 68, 273, "west")
        w.torch(255, 67, 283, "north")
        w.torch(257, 67, 283, "north")
        w.set(256, top, 272, 89)
        w.torch(256, 68, 271, "south")
        # V6b: wall torches every 4 on the outside of the west wall light
        # the path along it
        for z in (271, 275, 279, 283):
            w.torch(247, 67, z, "west")
        self.chest(249, 66, 271, [(0, TORCH, 8), (1, WOOD_SWORD, 1), (2, BOW, 1),
                                  (3, ARROW, 16)])
        self.walk.discard((249, 66, 271))

    # ------------------------------------------- 2.15 bridge and islet ---
    def bridge(self):
        w = self.w
        self.zones["causeway"] = (312, 209, 330, 215)
        for x in range(312, 331):
            for z in range(209, 216):
                self.reserved.add((x, z))
                for y in range(66, 72):
                    w.set(x, y, z, 0)
                if z in (209, 215):
                    w.set(x, 66, z, FENCE)
                else:
                    self.walk.add((x, 66, z))
        for x in (312, 318, 324, 330):
            w.torch(x, 67, 209, "floor")
        for x in (315, 321, 327):
            w.torch(x, 67, 215, "floor")
        for z in (211, 212, 213):
            w.set(319, 65, z, COBBLE)
            w.set(326, 65, z, COBBLE)
            w.piston(320, 65, z, "east", sticky=True)
            w.piston(325, 65, z, "west", sticky=True)
            w.set(321, 65, z, COBBLE)
            w.set(324, 65, z, COBBLE)
        # Pit x322..323, z211..213 (2 deep) + exit well at z=214.
        for x in (321, 322, 323, 324):
            for z in range(210, 216):
                w.set(x, 63, z, COBBLE)
                w.set(x, 64, z, COBBLE)
        for x in (322, 323):
            for z in (211, 212, 213, 214):
                w.set(x, 64, z, 0)
                w.set(x, 65, z, 0)
                self.walk.discard((x, 66, z))
                self.walk.add((x, 64, z))
            w.set(x, 65, 215, PLANKS)
            w.set(x, 66, 215, 0)
            self.walk.add((x, 66, 215))
        w.ladder(322, 64, 214, "east")
        w.ladder(322, 65, 214, "east")
        self.walk.discard((322, 64, 214))
        self.sign(323, 65, 214, "CLIMB THE / LADDER OUT", "north", wall=True)
        # Lever + wires (3.3).
        w.set(322, 65, 209, PLANKS)
        w.lever(322, 66, 209, "floor_ew")
        for x in range(319, 327):
            w.wire(x, 66, 210)
            self.walk.discard((x, 66, 210))
        for z in (211, 212, 213):
            w.wire(319, 66, z)
            w.wire(326, 66, z)
            self.walk.discard((319, 66, z))
            self.walk.discard((326, 66, z))
        self.post_sign(321, 67, 209, "PULL THE LEVER / PISTONS PUSH / THE "
                       "BRIDGE OUT / PULL IT BACK", "south")
        self.beacon(312, 208, "west", "PISTON BRIDGE / PULL THE LEVER / "
                    "TO CROSS")
        self.beacon(331, 208, "west", "ISLET / BED AND / PRACTICE")

    def islet(self):
        w = self.w
        self.shed("pavilion", 342, 210, 346, 214, 65, wall=(24, 0),
                  post="birch", doors=[(342, 212)], trim="light_blue",
                  ridge_block=(24, 0))
        for y in (66, 67, 68):
            w.set(344, y, 211, 24)
            w.set(344, y, 212, 24)
        w.set(344, 68, 213, 24)
        w.door(344, 66, 213, "east")
        w.bed(345, 66, 212, "north")
        w.torch(345, 67, 211, "south")
        w.torch(343, 68, 213, "north")
        self.chest(343, 66, 211, [(0, 29, 2), (1, COBBLE, 8), (2, 69, 1),
                                  (3, REDSTONE, 16)])
        # Practice bays (3.3 geometry, no lever/wire/pistons): feeders 334/341,
        # piston slots 335/340 (holes), bridge blocks 336/339, pit 337..338.
        self.zones["bays"] = (333, 217, 342, 220)
        for z in (218, 219):
            for x in range(334, 342):
                for y in range(63, 66):
                    w.set(x, y, z, 1 if y < 65 else 2)
            w.set(334, 65, z, COBBLE)
            w.set(341, 65, z, COBBLE)
            w.set(336, 65, z, COBBLE)
            w.set(339, 65, z, COBBLE)
            for x in (335, 340):
                w.set(x, 65, z, 0)
            for x in (337, 338):
                w.set(x, 64, z, 0)
                w.set(x, 65, z, 0)
                w.set(x, 63, z, COBBLE)
                self.walk.add((x, 64, z))
            for x in range(334, 342):
                if x not in (335, 337, 338, 340):
                    self.walk.add((x, 66, z))
                self.reserved.add((x, z))
        for x in (335, 340):
            for z in (218, 219):
                self.walk.add((x, 65, z))
        w.ladder(337, 64, 219, "north")
        w.ladder(337, 65, 219, "north")
        self.walk.discard((337, 64, 219))
        # V6b: the west lamp stands on the sand (ground 64); at y 65 it
        # hung one block over it
        self.lamp(333, 218)
        self.lamp(342, 219, 65)
        self.post_sign(338, 66, 221, "WIRE IT UP / LIKE THE BRIDGE", "north")
        self.reserved.add((338, 221))

    # ================================================================ V2 ===
    # V2 style pass (tmp/jobs/tutorial-1021/v2-style-guide.md): Beta 1.7.3
    # palette only (ids <= 96, no 1.8+ blocks): harbour town + castle hub.
    # Everything here is decoration: it never touches a stop's demo, gate,
    # sign, chest or circuit cell (those are placed first; the passes below
    # only write into free grass / air or into cells they own).

    PLAIN = (0, 31, 37, 38)

    def free_col(self, x, z, y, h=4):
        return all(self.bid(x, yy, z) in self.PLAIN for yy in range(y, y + h))

    def in_boxes(self, x, z, m=0, soft=False):
        boxes = list(self.footprints.values()) + list(self.zones.values())
        if soft:
            boxes += list(self.soft.values())
        return any(x1 - m <= x <= x2 + m and z1 - m <= z <= z2 + m
                   for (x1, z1, x2, z2) in boxes)

    def pixel(self, x, y, z, ch, palette):
        blk = palette.get(ch)
        if blk is not None:
            self.w.set(x, y, z, *blk)

    def details(self):
        """Mine (V6, after the lowering; pit floor y 62): a 3-wide x 3-high
        log-framed mouth with spruce props every 4 blocks (a slab-roofed
        timber porch out into the pit) and a cart track into the hill; coal
        and iron clusters on the pit faces at step height (iron only at
        y <= 63, so 'IRON: UNDER 64' stays true); a 3x3 open plank shed at
        the pit edge with furnace, crafting table and chest; a timber
        headframe with a torch over the ladder shaft, visible from the hub.
        Plus the striped wool awning over the smithy door."""
        w = self.w
        d = self.mine_dy
        fl = 65 + d                                # pit floor y (62)
        for x in (230, 231, 232):                  # 3-wide adit
            for z in (213, 214, 215):
                w.set(x, fl, z, COBBLE)
                for y in (fl + 1, fl + 2, fl + 3):
                    w.set(x, y, z, 0)
                self.walk.add((x, fl + 1, z))
        for zp in (215, 213, 219):                 # props every 4 (+ mouth)
            for x in (229, 233):
                for y in (fl + 1, fl + 2, fl + 3):
                    w.log(x, y, zp, "spruce")
                self.walk.discard((x, fl + 1, zp))
            for x in range(229, 234):
                w.log(x, fl + 4, zp, "spruce")
        for x in range(228, 235):                  # porch roof
            for z in range(216, 220):
                w.slab(x, fl + 5, z, "wood")
        for x in (229, 233):
            w.torch(x, fl + 2, 218, "north")
        for z in range(213, 220):
            w.rail(231, fl + 1, z, "ns")
            self.walk.discard((231, fl + 1, z))
        # ore clusters at step height on the pit faces
        ores = [(222, fl + 1, 215, 16), (223, fl + 1, 215, 16),
                (222, fl + 2, 215, 16), (223, fl + 2, 215, 16),
                (237, fl + 1, 215, 15), (238, fl + 1, 215, 15),
                (239, fl + 1, 216, 15), (239, fl + 1, 217, 15),
                (239, fl + 2, 216, 16), (239, fl + 2, 217, 16),
                (221, fl + 1, 217, 16), (221, fl + 2, 217, 16),
                (221, fl + 1, 218, 16), (221, fl + 2, 218, 16)]
        self.ore_clusters = 0
        for (x, y, z, ore) in ores:
            assert ore != 15 or y <= 63, "iron above 63 breaks the lesson"
            if self.bid(x, y, z) in (1, 16, 15) and any(
                    self.bid(x + a, y, z + b) == 0
                    for (a, b) in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                w.set(x, y, z, ore)
                self.ore_clusters += 1
        # 3x3 open plank shed at the pit edge by the ramp foot
        sx, sz = 223, 218
        for x in (sx, sx + 2):
            for z in (sz, sz + 2):
                for y in (fl + 1, fl + 2, fl + 3):
                    w.log(x, y, z, "spruce")
        # V6b: an all-slab roof (its planks centre was a dark spawn
        # platform over the shed)
        for x in range(sx - 1, sx + 4):
            for z in range(sz - 1, sz + 4):
                w.slab(x, fl + 4, z, "wood")
        for x in range(sx, sx + 3):
            for z in range(sz, sz + 3):
                self.reserved.add((x, z))
        w.furnace(sx + 1, fl + 1, sz, "south")
        w.set(sx, fl + 1, sz + 1, 58)
        self.chest(sx + 2, fl + 1, sz + 1, [(0, COBBLE, 8), (1, STICK, 4),
                                            (2, COAL, 4)])
        self.post_sign(sx + 3, fl + 1, sz + 2, "DIG STONE, / CRAFT A STONE / "
                       "PICK, SMELT / IRON HERE", "south")
        self.walk.discard((sx + 3, fl + 1, sz + 2))
        # headframe over the ladder shaft (ladder column (232, *, 207))
        top = 78
        for (x, z) in ((231, 206), (233, 206), (231, 208), (233, 208)):
            g = self.ground(x, z)
            w.log(x, g + 1, z, "spruce")           # footing
            for y in range(g + 2, top):
                w.set(x, y, z, FENCE)              # thin timber legs
            self.reserved.add((x, z))
        for x in (231, 232, 233):                  # head frame + pulley
            for z in (206, 207, 208):
                if x == 232 or z == 207:
                    w.log(x, top, z, "spruce")
                else:
                    w.set(x, top, z, PLANKS)
        w.set(232, top - 1, 207, FENCE)
        w.torch(232, top + 1, 207, "floor")
        for x in (228, 229, 230, 231):
            if self.ground(x, 207) == 72:
                w.rail(x, 73, 207, "ew")
        for x in range(285, 292):
            w.wool(x, 68, 212, "orange" if x % 2 else "white")
        # V6b: a lamp hung in the awning over the door (like the market
        # stalls): it lights the awning top, which was a dark spawn strip
        w.set(288, 68, 212, 89)
        for x in (285, 291):
            w.set(x, 66, 212, FENCE)
            w.set(x, 67, 212, FENCE)


    # ------------------------------------------------------ harbour ---
    def harbour(self):
        """Stop 1 harbour: a planks pier on spruce piles south of the quay
        with a T-head, log bollards and fence rails, a moored sailing boat
        built from blocks (boats are entities), and cargo on the quay."""
        w = self.w
        self.zones["pier"] = (197, 308, 213, 327)
        deck = {(x, z) for x in range(205, 208) for z in range(308, 326)}
        # V7: the T-head is 3 rows deep (z 324..326), so its middle row is
        # a walkway with the boat kit at the east end (a launch gap in the
        # north rail onto the lane between the stem and the lane posts) and
        # the fishing kit at the west end (cast over the south rail).
        deck |= {(x, z) for x in range(202, 213) for z in (324, 325, 326)}
        for (x, z) in deck:
            w.set(x, 64, z, PLANKS)
            for y in range(65, 70):
                if self.bid(x, y, z) in (8, 9):
                    w.set(x, y, z, 0)
            self.walk.add((x, 65, z))
            self.reserved.add((x, z))
        piles = {(x, z) for (x, z) in deck
                 if (x in (205, 207) and z % 3 == 1 and z < 324)
                 or (z == 326 and x in (202, 207, 212))
                 or (z in (324, 325) and x in (202, 212))}
        for (x, z) in piles:
            y = 63
            while y > 40 and self.bid(x, y, z) in (0, 8, 9, 12):
                w.log(x, y, z, "spruce")
                y -= 1
        boarding = {(205, 317), (205, 318), (209, 324), (210, 324)}
        for (x, z) in sorted(deck):
            if z < 310 or (x, z) in boarding:
                continue
            if all((x + dx, z + dz) in deck
                   for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                continue
            if (x, z) in piles:
                w.log(x, 65, z, "spruce")
            else:
                w.set(x, 65, z, FENCE)
            self.walk.discard((x, 65, z))
        for (x, z) in ((205, 311), (207, 315), (205, 321), (207, 320),
                       (204, 324), (208, 324), (211, 324), (204, 326),
                       (209, 326)):
            if self.bid(x, 65, z) == FENCE:
                w.torch(x, 66, z, "floor")
        self.pier_kits()
        self.ship()
        # V3 quay crane: log post on the quay, fence jib over the water,
        # fence chain with a crate (planks) on the hook. V6: moved to x 203
        # so it loads the ship (the quay's east end is the map walkway);
        # the crate hangs at y 68, clear of heads on the quarterdeck.
        for y in range(65, 71):
            w.log(203, y, 307, "spruce")
        self.walk.discard((203, 65, 307))
        for z in range(308, 312):
            w.set(203, 70, z, FENCE)
        w.set(203, 69, 311, FENCE)
        w.set(203, 68, 311, PLANKS)
        # Cargo on the quay: crates (planks) and a barrel (log); V6 moved
        # west of the chest (the east end is the map walkway).
        w.set(195, 65, 307, 17)
        w.set(196, 65, 307, PLANKS)
        w.set(196, 66, 307, PLANKS)
        for p in ((195, 65, 307), (196, 65, 307)):
            self.walk.discard(p)

    def pier_kits(self):
        """V7 boats and fishing on the T-head walkway (z 325). Each end
        bollard (a 2-high spruce log with a torch on top) carries the
        kit's sign above its chest. Boats are items only (ItemBoat places
        the entity on water); two lane posts east of the stem mark the
        water lane north of the launch gap."""
        w = self.w
        for (bx, cx, facing) in ((202, 203, "east"), (212, 211, "west")):
            w.log(bx, 66, 325, "spruce")
            w.torch(bx, 67, 325, "floor")
            self.walk.discard((cx, 65, 325))
        self.chest(203, 65, 325, [(0, FISHING_ROD, 1), (1, FISHING_ROD, 1),
                                  (2, FISH_COOKED, 1), (3, FISH_COOKED, 1)])
        self.sign(203, 66, 325, "CAST THE ROD / OVER THE RAIL, / WAIT FOR A "
                  "/ SPLASH, REEL IN", "east", wall=True)
        self.chest(211, 65, 325, [(0, BOAT, 1), (1, BOAT, 1), (2, PLANKS, 5)])
        self.sign(211, 66, 325, "PLACE A BOAT / ON THE WATER / AT THE GAP, "
                  "/ GET IN, ROW", "west", wall=True)
        for z in (317, 321):
            assert self.bid(212, 63, z) == 9, "lane post must stand in water"
            y = 65
            while y > 40 and self.bid(212, y, z) in (0, 8, 9, 12):
                w.log(212, y, z, "spruce")
                y -= 1
            w.torch(212, 66, z, "floor")
            self.reserved.add((212, z))

    def ship(self):
        """V3 harbour ship moored west of the pier (keel x=202, bow south):
        15 long x 5 wide. Spruce keel, plank strakes and bottom, a 1-deep
        sealed bilge under a plank deck, spruce gunwale (gap at the pier
        boarding point), raised quarterdeck at the stern with a fence rail,
        tapered stair bow, two fence masts with white wool sails on fence
        yards and a red pennant."""
        w = self.w
        sx = 202
        for z in range(309, 324):
            for x in range(sx - 2, sx + 3):
                for y in range(62, 70):
                    w.set(x, y, z, 9 if y <= 63 else 0)
        for z in range(309, 321):                      # full section
            w.log(sx, 61, z, "spruce")
            for x in range(sx - 1, sx + 2):
                w.set(x, 62, z, PLANKS)
                w.set(x, 63, z, 0)
                w.set(x, 64, z, PLANKS)
            for x in (sx - 2, sx + 2):
                w.set(x, 62, z, PLANKS)
                w.set(x, 63, z, PLANKS)
                w.set(x, 64, z, PLANKS)
                if not (x == sx + 2 and z in (317, 318)):
                    w.log(x, 65, z, "spruce")
            for x in range(sx - 1, sx + 2):
                self.walk.add((x, 65, z))
        for x in range(sx - 2, sx + 3):                # stern transom
            for y in range(62, 66):
                w.set(x, y, 309, PLANKS)
        for z in range(310, 313):                      # quarterdeck
            for x in range(sx - 1, sx + 2):
                w.set(x, 65, z, PLANKS)
                self.walk.discard((x, 65, z))
                self.walk.add((x, 66, z))
            for x in (sx - 2, sx + 2):
                w.set(x, 66, z, FENCE)
        for x in range(sx - 2, sx + 3):
            w.set(x, 66, 309, FENCE)
        for x in range(sx - 1, sx + 2):
            w.stairs(x, 65, 313, "north")
        # bow: z=321 narrows to 3, z=322 stem, z=323 stair prow
        w.log(sx, 61, 321, "spruce")
        for x in (sx - 1, sx + 1):
            for y in (62, 63, 64):
                w.set(x, y, 321, PLANKS)
            w.stairs(x, 65, 321, "north")
        for y in (62, 63, 64):
            w.set(sx, y, 321, PLANKS)
        self.walk.add((sx, 65, 321))
        for y in (62, 63, 64):
            w.set(sx, y, 322, PLANKS)
        w.stairs(sx, 65, 322, "north")
        w.stairs(sx, 64, 323, "north")
        # masts, yards, sails, pennant
        for (mz, top, sail_z, xs, ys) in ((315, 74, 316, range(sx - 2, sx + 3),
                                           range(67, 72)),
                                          (319, 71, 320, range(sx - 1, sx + 2),
                                           range(67, 70))):
            for y in range(65, top + 1):
                w.set(sx, y, mz, FENCE)
            w.set(sx, 64, mz, 89)          # mast step: glowstone deck light
            self.walk.discard((sx, 65, mz))
            for x in xs:
                w.set(x, ys[-1] + 1, sail_z, FENCE)
                for y in ys:
                    w.wool(x, y, sail_z, "white")
        w.wool(sx, 75, 315, "red")
        w.set(sx, 76, 315, FENCE)      # V6b: masthead finial (no spawn top)
        # stern lantern (jack-o'-lantern on the transom rail line)
        w.pumpkin(sx, 67, 309, "south", lit=True)
        w.set(sx, 66, 309, PLANKS)

    # --------------------------------------------------- lighthouse ---
    def lighthouse(self):
        """Red-and-white wool lighthouse on the west headland: glowstone
        lantern behind glass, smooth-slab gallery with a fence rail."""
        w = self.w
        cx, cz = 178, 300
        g = self.ground(cx, cz)
        self.zones["lighthouse"] = (174, 296, 182, 304)
        for dx in range(-3, 4):
            for dz in range(-3, 4):
                d2 = dx * dx + dz * dz
                x, z = cx + dx, cz + dz
                if d2 <= 10:
                    for y in range(self.ground(x, z) + 1, g + 2):
                        w.set(x, y, z, COBBLE if (H(x, z, y) % 5) else 48)
                if d2 <= 5:
                    for y in range(g + 2, g + 14):
                        band = ((y - g - 2) // 3) % 2
                        w.wool(x, y, z, "red" if band else "white")
                if d2 <= 10:
                    w.slab(x, g + 14, z, "stone", double=True)
                    if d2 > 5:
                        w.set(x, g + 15, z, FENCE)
                if d2 <= 2:
                    for y in (g + 15, g + 16):
                        w.set(x, y, z, 89 if d2 == 0 else 20)
                    w.slab(x, g + 17, z, "stone", double=True)
        w.set(cx, g + 18, cz, 89)      # V6: glowstone beacon on the cap
        # V6b: the beacon is 18 up and lights nothing at the foot; one wall
        # torch on each face of the bottom wool course lights the base ring
        # and the headland round it
        for (dx, dz, f) in ((0, -3, "north"), (0, 3, "south"),
                            (-3, 0, "west"), (3, 0, "east")):
            w.torch(cx + dx, g + 2, cz + dz, f)
        self.lighthouse_g = g
        self.sealed.append((cx - 3, cz - 3, cx + 3, cz + 3, g + 15, g + 15,
                            "torch"))

    # ---------------------------------------------------- CAVEX sign ---
    # V4: built the way the Xbox 360 Edition tutorial world (TU1-TU46, the
    # TU3 world included) builds its giant logo, only spelling CAVEX: blocky
    # wide letters in the classic logo style, 7 tall with 2-block strokes and
    # 1-block gaps, smooth stone faces set flush in a black wool panel
    # (1-block black wool border round the top and sides), every gap and
    # counter recessed 1 block to the black wool behind, on a thick black wool
    # plinth whose lip projects in front; torches along the top edge and
    # along the lip. Letterforms follow the approved CAVEX title wordmark
    # (chamfered outer corners) with the V2 creeper face kept in the A (maintainer
    # request): 'B' cells are black wool eyes and mouth set flush in the stone.
    GLYPHS = {
        "C": [".#########", "##########", "##########", "###.......",
              "###.......", "###.......", "###.......", "###.......",
              "###.......", "###.......", "##########", "##########",
              ".#########"],
        "A": [".########.", "##########", "##BB##BB##", "##BB##BB##",
              "##########", "####BB####", "###BBBB###", "###BBBB###",
              "###B##B###", "###B##B###", "###B##B###", "###B##B###",
              "###B##B###"],
        "V": ["###....###", "###....###", "###....###", "###....###",
              "###....###", "###....###", "###....###", "####..####",
              "####..####", ".########.", ".########.", "..######..",
              "...####..."],
        "E": [".#########", "##########", "##########", "###.......",
              "###.......", "########..", "########..", "########..",
              "###.......", "###.......", "##########", "##########",
              ".#########"],
        "X": ["###....###", "###....###", ".###..###.", "..######..",
              "...####...", "...####...", "...####...", "...####...",
              "...####...", "..######..", ".###..###.", "###....###",
              "###....###"],
    }
    # 10 wide x 13 tall, 3-block strokes, letters on an 11 pitch (the 1-block
    # gap carries the right-hand bevel), like the real wordmark's squarish
    # heavy glyphs. A 3-row dark-grey wool underside and 1-column right
    # bevel give the extruded look.
    #
    # V6 placement: the sign hangs over the WEST SEA, facing east toward the
    # island, so its skylight shadow falls on water (never on the plaza, a
    # road or a yard). Its letter plane is x = CAVEX_XF, centred on z =
    # CAVEX_ZC (near the plaza's west-east axis: it is framed head-on from
    # the plaza's west gate, 83 blocks, and the whole sign lies inside the
    # Wii's 5-chunk near view from both the gate and spawn) and up ahead in
    # the spawn view (47..95 blocks). Its bottom (the
    # panel's lower margin, y CAVEX_BASE - 1 = 74) sits 11 above the sea
    # (water top y 63), below the keep's tower crenels (y 82); maintainer
    # 10-31 asked for it closer to the ground (was base 93, bottom y 92).
    # Line of sight at base 75 (letter-face cells, rays from eye height):
    # ~95% visible from the plaza's west gate (exit 12); from the exact
    # spawn eye about half is hidden behind gate 1's numeral board, as it
    # was at base 93 (~61%).
    # V8: letter plane x 162 (was 161): the new stone back skin (x 160)
    # stays over open sea and inside the island overview
    CAVEX_XF, CAVEX_ZC, CAVEX_BASE = 161, 253, 75

    def cavex_sign(self):
        """V6 CAVEX landmark (TU3 tutorial-logo construction, slimmed): the
        stone letters (creeper-face A kept: 'B' cells are black wool) and
        their grey wool bevel stand in the front plane; behind them a
        1-block-thick black wool panel with a 1-block margin round the glyph
        box; a slim 1-high black wool under-ledge (2 deep) carries the lip
        torches; the bulky top and bottom ledge bars of V5 are gone. Torches
        stand on the panel's top edge every 4 blocks; every other dark
        spawnable surface (letter tops, counter floors) gets a floor torch
        from the sealed-room pass, so the whole sign is mob-safe.

        Text runs left to right as seen by a viewer east of the sign looking
        west: left = south (+z), so letter column u maps to z = ZL - u."""
        w = self.w
        xf, B = self.CAVEX_XF, self.CAVEX_BASE
        rows = len(self.GLYPHS["C"])                     # 13
        width = 5 * 11 - 1                               # 54 glyph columns
        zl = self.CAVEX_ZC + width // 2                  # z of column u = 0

        def Z(u):
            return zl - u
        cells, black, stone = set(), set(), set()
        lu = 0
        for ch in "CAVEX":
            for r, row in enumerate(self.GLYPHS[ch]):
                for c, k in enumerate(row):
                    yy = B + rows - 1 - r
                    if k in "#B":
                        cells.add((lu + c, yy))
                    if k == "#":
                        stone.add((lu + c, yy))
                    if k == "B":
                        black.add((lu + c, yy))
            lu += 11
        bevel = set()
        for (u, y) in stone:                             # 3D underside + side
            if (u, y - 1) not in stone:
                g = 1                                    # open rows below
                while g < 40 and (u, y - g) not in stone and y - g >= B:
                    g += 1
                g -= 1
                if (u, y - g - 1) in stone:              # enclosed gap: keep
                    g = max(0, g - 2)                    # >= 2 clear rows
                for d in range(1, min(3, g) + 1):
                    bevel.add((u, y - d))
            if (u + 1, y) not in stone:
                bevel.add((u + 1, y))
                bevel.add((u + 1, y - 1))
        bevel -= cells
        bevel = {(u, y) for (u, y) in bevel if y >= B and u <= width}
        ua, ub = -1, width                               # panel margin cols
        ylo, yhi = B - 1, B + rows                       # panel margin rows
        for u in range(ua, ub + 1):
            z = Z(u)
            for y in range(ylo, yhi + 1):
                w.set(xf - 1, y, z, 35, 15)              # panel, 1 thick
                if (u, y) in black:
                    w.set(xf, y, z, 35, 15)
                elif (u, y) in cells:
                    w.set(xf, y, z, 1)
                elif (u, y) in bevel:
                    w.set(xf, y, z, 35, 7)
                else:
                    w.set(xf, y, z, 0)                   # 1-block recess
            # slim under-ledge: 1 high, under the letter plane and 1 out
            w.set(xf, ylo, z, 35, 15)
            w.set(xf + 1, ylo, z, 35, 15)
        z1, z2 = Z(ub), Z(ua)
        self.cavex_box = (xf - 4, z1 - 3, xf + 4, z2 + 3, ylo - 3)
        # every spawnable surface on the sign (letter tops, counter and recess
        # floors, ledge, top edge) is lit by the sealed-room pass (torch mode)
        self.sealed.append((xf - 1, z1, xf + 1, z2, ylo, yhi + 1, "torch"))
        for u in range(0, width, 2):                     # lip torches
            w.torch(xf + 1, ylo + 1, Z(u), "floor")
        for u in list(range(ua, ub + 1, 4)) + [ub]:      # top-edge torches
            w.torch(xf - 1, yhi + 1, Z(u), "floor")

    # ----------------------------------------------------- castle ---
    # V6 hanging banner (top row first): black header, red field, white
    # cross, swallow-tail foot ('.' keeps the tower wall).
    BANNER = ["KKK", "RRR", "RWR", "WWW", "RWR", "RRR", "R.R"]

    def castle_keep(self, x1, z1, x2, z2, fy, wall_h, crest=True,
                    turrets="all", deck="glow"):
        """Redstone hall as the castle keep: smooth double-slab string
        course, mossy weathering, flat roof over a glowstone-lit ceiling,
        crenellated parapet, four corner turrets and a gatehouse front with
        exit 13's wool numeral and two hanging banners (V6)."""
        w = self.w
        roof = fy + wall_h + 1
        for x in range(x1, x2 + 1):
            for z in range(z1, z2 + 1):
                if not (x in (x1, x2) or z in (z1, z2)):
                    w.set(x, roof, z, COBBLE)
                    continue
                for y in range(fy + 1, fy + wall_h):
                    if self.bid(x, y, z) == COBBLE and H(x, z, y) % 7 == 0:
                        w.set(x, y, z, 48)
                if self.bid(x, fy + wall_h, z) == COBBLE:
                    w.slab(x, fy + wall_h, z, "stone", double=True)
                w.set(x, roof, z, COBBLE)
                w.set(x, roof + 1, z, COBBLE)
                along = (x - x1) if z in (z1, z2) else (z - z1)
                if along % 2 == 0:
                    w.set(x, roof + 2, z, COBBLE)
        if not crest:
            # the lab block keeps the V2 4x4 roof turrets on its north side
            for (ax, az) in [(x1, z2 - 3), (x2 - 3, z2 - 3)]:
                for x in range(ax, ax + 4):
                    for z in range(az, az + 4):
                        for y in range(roof + 1, roof + 6):
                            w.set(x, y, z, 48 if H(x, z, y) % 5 == 0 else COBBLE)
                        ring = x in (ax, ax + 3) or z in (az, az + 3)
                        if ring and (x + z) % 2 == 0:
                            w.set(x, roof + 6, z, COBBLE)
                w.torch(ax + 1, roof + 6, az + 1, "floor")
                w.torch(ax + 2, roof + 6, az + 2, "floor")
            self.sealed.append((x1, z1, x2, z2, roof + 1, roof + 9, "torch"))
            return
        # V3 keep: 5x5 corner towers 3 blocks above the parapet. The two
        # front (south) towers stand centred on the corners, i.e. 2 blocks
        # forward of both walls; the north pair cannot project north (road
        # L5 runs along that face) and project west/east only. Inside the
        # hall footprint a tower starts above the roof, so no interior cell
        # (clock, chest, torches) is touched.
        towers = [(x1 - 2, z2 - 2), (x2 - 2, z2 - 2), (x1 - 2, z1), (x2 - 2, z1)]
        for (ax, az) in towers:
            for x in range(ax, ax + 5):
                for z in range(az, az + 5):
                    inside = x1 < x < x2 and z1 < z < z2
                    y0 = roof + 1 if inside else fy + 1
                    for y in range(y0, roof + 6):
                        w.set(x, y, z, 48 if H(x, z, y) % 5 == 0 else COBBLE)
                    ring = x in (ax, ax + 4) or z in (az, az + 4)
                    if ring and (x + z) % 2 == 0:
                        w.set(x, roof + 6, z, COBBLE)
                    if inside or (x1 <= x <= x2 and z1 <= z <= z2):
                        continue
                    self.reserved.add((x, z))
            w.torch(ax + 2, roof + 6, az + 2, "floor")
            self.sealed.append((ax, az, ax + 4, az + 4, roof + 6, roof + 7,
                                "torch"))
        # Recessed gatehouse: projects 2 blocks into the plaza in front of the
        # hall door; stepped arch over the 3-wide passage; the exit numeral
        # on its front above the arch; crenellated top.
        mid = (x1 + x2) // 2
        gz1, gz2 = z2 + 1, z2 + 2
        gtop = roof + 8
        for x in range(mid - 5, mid + 6):
            for z in (gz1, gz2):
                for y in range(fy + 1, gtop + 1):
                    w.set(x, y, z, 48 if H(x, z, y) % 9 == 0 else COBBLE)
                if (x - mid) % 2 == 0 or x in (mid - 5, mid + 5):
                    w.set(x, gtop + 1, z, COBBLE)
                self.reserved.add((x, z))
        # V6b: the gatehouse deck lies outside the hall's sealed box; light
        # it too (floor torches), it overlooks the plaza
        self.sealed.append((mid - 5, gz1, mid + 5, gz2, gtop + 1, gtop + 2,
                            "torch"))
        for x in range(mid - 1, mid + 2):
            for z in (gz1, gz2):
                for y in (fy + 1, fy + 2):
                    w.set(x, y, z, 0)
                w.set(x, fy + 3, z, 0)
                w.slab(x, fy + 4, z, "stone", double=True)
        for z in (gz1, gz2):
            w.stairs(mid - 1, fy + 3, z, "west", "cobblestone")
            w.stairs(mid + 1, fy + 3, z, "east", "cobblestone")
        # V6: the flat wool crest is gone. Exit 13's numeral is set flush in
        # the gatehouse front above the arch, and two narrow hanging banners
        # (3 wide x 7 tall, red / white / black wool) hang on the faces of
        # the twin front towers that flank the gate.
        self.numeral_board(mid, gz2, fy + 5, "south", "13", "red", flush=True)
        pal = {"K": "black", "R": "red", "W": "white"}
        for (ax, az) in towers[:2]:
            for r, row in enumerate(self.BANNER):
                for c, ch in enumerate(row):
                    if ch in pal:
                        w.wool(ax + 1 + c, fy + 9 - r, az + 4, pal[ch])
                        self.no_window.add((ax + 1 + c, fy + 9 - r, az + 4))
        # V6b: a row of wall torches along the keep front (each tower's
        # corners, either side of the gatehouse) lights the plaza's north
        # strip under the banners
        for tx in (x1 - 2, x1 + 2, mid - 5, mid + 5, x2 - 2, x2 + 2):
            w.torch(tx, fy + 2, z2 + 3, "south")
        self.sealed.append((x1, z1, x2, z2, roof + 1, roof + 9, deck))

    def castle_bands(self):
        """V3 cobble-stair band (corbel course) one block out from the keep
        and lab walls under the crenels, at roof+1 (run after the hill lift,
        because the band cells outside the walls are not in the lifted
        columns). Only fills air cells."""
        w = self.w
        for name in ("hall", "lab"):
            x1, z1, x2, z2 = self.footprints[name]
            meta = self.house_meta[name]
            y = meta["fy"] + meta["wall_h"] + 2
            for x in range(x1 - 1, x2 + 2):
                for (z, asc) in ((z1 - 1, "south"), (z2 + 1, "north")):
                    if w.get(x, y, z)[0] == 0 and x1 <= x <= x2:
                        w.stairs(x, y, z, asc, "cobblestone")
            for z in range(z1, z2 + 1):
                for (x, asc) in ((x1 - 1, "east"), (x2 + 1, "west")):
                    if w.get(x, y, z)[0] == 0:
                        w.stairs(x, y, z, asc, "cobblestone")

    # ------------------------------------------------------- plaza ---
    def plaza_v2(self):
        """Hub square: patterned floor (stone field, smooth double-slab
        ring, cobble ring, sandstone avenues), a four-pool fountain garden
        round the dais with lit obelisks, market stalls with striped wool
        awnings, planted trees."""
        w = self.w
        for x in range(245, 268):
            for z in range(245, 268):
                dx, dz = x - 256, z - 256
                r = max(abs(dx), abs(dz))
                if r <= 1:
                    continue
                # V8: one plain stone field with the sandstone avenues (the
                # trail's own surface) running to the four exits; the
                # double-slab ring, cobble ring and cobble specks are gone
                if (abs(dx) <= 1 or abs(dz) <= 1) and r >= 5:
                    b = 24
                else:
                    b = 1
                w.set(x, 65, z, b)
        for dx in range(-5, 6):
            for dz in range(-5, 6):
                x, z = 256 + dx, 256 + dz
                r = max(abs(dx), abs(dz))
                avenue = abs(dx) <= 1 or abs(dz) <= 1
                if r in (3, 4) and not avenue:
                    w.set(x, 64, z, 82)
                    w.water_source(x, 65, z)
                    self.walk.discard((x, 66, z))
                elif r == 5 and not avenue:
                    w.slab(x, 66, z, "stone")
                    self.walk.discard((x, 66, z))
                elif r == 2:
                    w.slab(x, 65, z, "stone", double=True)
        for sx in (-4, 4):
            for sz in (-4, 4):
                x, z = 256 + sx, 256 + sz
                for y in (65, 66, 67):
                    w.slab(x, y, z, "stone", double=True)
                w.set(x, 68, z, 89)
                w.slab(x, 69, z, "stone")
        # V3 four-way signpost: a log post keeps the four hub signs at y=68
        # on its faces; above each sign a plank arm ends in the wool colour
        # of that way (north hall red, west yard black, south lab purple,
        # east bridge blue); a jack-o'-lantern tops the post.
        for y in (67, 68, 69, 70):
            w.log(256, y, 256, "spruce")
        # V8: plain plank arms (the coloured wool arm tips were incidental
        # colour at the central junction; the signs name each way)
        for (dx, dz) in ((0, -1), (-1, 0), (0, 1), (1, 0)):
            w.set(256 + dx, 69, 256 + dz, PLANKS)
        w.pumpkin(256, 71, 256, "south", lit=True)
        # Market stalls (goods are plain blocks, no tile entities).
        stalls = [((246, 247), ("red", "white"), [(PLANKS, 0), (86, 0), (92, 0)]),
                  ((264, 247), ("blue", "white"), [(25, 0), (84, 0), (25, 0)]),
                  ((246, 263), ("lime", "white"), [None, None, None]),
                  ((264, 263), ("yellow", "white"), [(47, 0), (58, 0), (47, 0)])]
        for (sx, sz), (c1, c2), goods in stalls:
            c1 = "brown"            # V8: one muted awning colour
            for x in range(sx, sx + 3):
                for z in range(sz, sz + 3):
                    if x in (sx, sx + 2) and z in (sz, sz + 2):
                        w.set(x, 66, z, FENCE)
                        w.set(x, 67, z, FENCE)
                    w.wool(x, 68, z, c1 if (x - sx) % 2 == 0 else c2)
            # V6b: the goods stand on a double-slab counter between the
            # posts, not on the paving; a canopy lamp hangs over each
            # aisle (the counter would block a lamp hung over its middle)
            for lz in (sz, sz + 2):
                w.set(sx + 1, 68, lz, 89)
            for i, blk in enumerate(goods):
                w.slab(sx + i, 66, sz + 1, "stone", double=True)
                self.walk.discard((sx + i, 66, sz + 1))
                if blk is None:
                    continue
                if blk[0] == 25:
                    w.noteblock(sx + i, 67, sz + 1, 5)
                else:
                    w.set(sx + i, 67, sz + 1, *blk)
        # V7 map kiosk (the lime stall): compass, clock and map materials
        # in a chest on the counter between two recipe signs facing the
        # plaza (Beta: compass = 4 iron + redstone, clock = 4 gold +
        # redstone, map = 8 paper round a compass).
        self.sign(246, 67, 264, "FIND SPAWN: / A COMPASS / POINTS TO THE / "
                  "LANDING BEACH", "north")
        self.chest(247, 67, 264, [(0, COMPASS, 1), (1, CLOCK, 1),
                                  (2, PAPER, 8), (3, IRON_INGOT, 4),
                                  (4, GOLD_INGOT, 4), (5, REDSTONE, 2)])
        self.sign(248, 67, 264, "MAKE A MAP: / 8 PAPER ROUND / A COMPASS. / "
                  "A CLOCK = TIME", "north")
        # V3 planting beds (no trees: the gate sightlines stay open): a
        # sandstone-slab kerbed grass bed with a clipped oak bush and flowers.
        for (tx, tz) in ((250, 250), (262, 250), (250, 262), (262, 262)):
            for dx in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    w.set(tx + dx, 65, tz + dz, 2)
                    self.walk.discard((tx + dx, 66, tz + dz))
            for (dx, dz, f) in ((-1, -1, "rose"), (1, 1, "rose"),
                                (1, -1, "dandelion"), (-1, 1, "dandelion")):
                w.flower(tx + dx, 66, tz + dz, f)
            w.log(tx, 66, tz)
            for (dx, dy, dz) in ((1, 0, 0), (-1, 0, 0), (0, 0, 1), (0, 0, -1),
                                 (0, 1, 0), (1, 1, 0), (-1, 1, 0), (0, 1, 1),
                                 (0, 1, -1), (0, 2, 0)):
                w.leaves(tx + dx, 66 + dy, tz + dz)

    # -------------------------------------------------------- town ---
    def town(self):
        """Market street along LE (the hub's east road): four half-timbered
        town houses (closed doors, lit, a little furniture) and a well."""
        w = self.w
        # V3 distinct silhouettes, V6 distinct USES: a hip-roofed sandstone
        # house; an open-front workshop (one tall storey, so its ridge
        # dropped 2) with a furnace whose chimney rises through the roof and
        # a cobble apron; a cobble-based spruce frame with white wool infill
        # (ridge along z); a LOW birch cottage (2-high walls) with a fenced
        # flower garden.
        # V8 (less clutter): town1 (family home) and town3 (study) taught
        # nothing and are gone; their lots stay open lawn (soft boxes keep
        # the tree scatter out). The workshop, the low cottage + garden, the
        # windmill and the islet pavilion are the only non-stop houses.
        self.soft["town1_lot"] = (270, 245, 278, 253)
        self.soft["town3_lot"] = (270, 259, 279, 267)
        houses = [
            ("town2", 285, 245, 292, 252, dict(post="oak", ridge="x",
             wall_h=5, chimney=(290, 246)), None),
            ("town4", 284, 260, 290, 266, dict(post="birch", mid_posts=3,
             wall_h=2, chimney=(289, 265), chimney_block=48),
             (287, 260, "south")),
        ]
        for name, x1, z1, x2, z2, kw, door in houses:
            kw.setdefault("lower", (COBBLE, 0))
            self.shed(name, x1, z1, x2, z2, 65,
                      doors=[door[:2]] if door else [], **kw)
            if door is None:
                continue
            dx, dz, f = door
            w.door(dx, 66, dz, f)
            # V6b: every house has a hearth (a furnace at the chimney foot,
            # its cobble flue rising into the stack) and a table; the rest
            # fits its use. town1 is a family home (bed, crafting table),
            # town3 a study (bookshelves), town4 a cottage (bed).
            cx, cz = kw["chimney"]
            wall_top = 65 + kw.get("wall_h", 3)
            w.furnace(cx, 66, cz, "south" if cz == z1 + 1 else "north")
            for y in range(67, wall_top + 1):
                w.set(cx, y, cz, COBBLE)
            w.set(x2 - 2, 66, (z1 + z2) // 2, FENCE)
            w.slab(x2 - 2, 67, (z1 + z2) // 2, "wood")
            # wall torches at head height instead of glowstone set in the
            # floor, and a torch either side of the front door
            mid = (z1 + z2) // 2
            w.torch(x1 + 1, 67, mid, "east")
            w.torch(x2 - 1, 67, mid, "west")
            out = "south" if dz == z2 else "north"
            oz = dz + (1 if dz == z2 else -1)
            for tx in (dx - 1, dx + 1):
                w.torch(tx, 67, oz, out)
        # street lights: a torch on each house corner that faces the
        # street, and a lantern in the gap by the well
        for (x, z, f) in ((285, 253, "south"),
                          (292, 253, "south"),
                          (284, 259, "north"), (290, 259, "north")):
            w.torch(x, 67, z, f)
        self.lamp(281, 253, 65)
        # town4 (door north): bed in the south-west corner
        w.bed(285, 66, 264, "south")
        # Workshop (town2): open front between log posts, furnace + table at
        # the back, the chimney stack starts right on the furnace.
        for x in range(286, 292):
            for y in (66, 67, 68):
                if x in (288, 289):
                    w.log(x, y, 252, "oak")
                else:
                    w.set(x, y, 252, 0)
        w.furnace(290, 66, 246, "south", lit=True,
                  items=[(1, COAL, 4)], burn_time=1600)
        for y in range(67, 71):
            w.set(290, y, 246, COBBLE)
        w.set(287, 66, 246, 58)
        w.set(286, 66, 246, 58)
        w.set(291, 66, 249, FENCE)
        w.slab(291, 67, 249, "wood")
        self.sign(288, 67, 246, "WORKSHOP: / CRAFT AND / SMELT HERE",
                  "south", wall=True)
        for x in range(285, 293):
            for z in (253, 254):
                if self.ground(x, z) == 65 and self.bid(x, 65, z) == 2:
                    w.set(x, 65, z, COBBLE)
                    self.walk.add((x, 66, z))
                    self.reserved.add((x, z))
        # V6b: wall torches instead of glowstone set in the floor, and a
        # torch on each face of the front pillar (workshop and apron)
        w.torch(286, 67, 249, "east")
        w.torch(291, 67, 248, "west")
        w.torch(288, 67, 251, "north")
        w.torch(289, 67, 253, "south")
        # Garden beside the low cottage (town4): fence ring, flower rows.
        self.zones["garden"] = (279, 260, 283, 266)
        for x in range(279, 284):
            for z in range(260, 267):
                self.reserved.add((x, z))
                for y in range(66, 70):
                    w.set(x, y, z, 0)
                if x in (279, 283) or z in (260, 266):
                    if (x, z) != (281, 260):
                        w.set(x, 66, z, FENCE)
                    continue
                w.set(x, 65, z, 2)
                kind = ("rose", "dandelion")[(z + x) % 2]
                if z == 263:
                    w.tall_grass(x, 66, z, "fern")
                else:
                    w.flower(x, 66, z, kind)
        # Well.
        self.footprints["well"] = (279, 247, 282, 250)
        for x in range(279, 283):
            for z in range(247, 251):
                inner = 280 <= x <= 281 and 248 <= z <= 249
                self.reserved.add((x, z))
                for y in range(66, 72):
                    w.set(x, y, z, 0)
                if inner:
                    w.set(x, 63, z, 82)
                    w.water_source(x, 64, z)
                    w.water_source(x, 65, z)
                else:
                    w.set(x, 65, z, COBBLE)
                    w.set(x, 66, z, COBBLE if H(x, z) % 4 else 48)
                    if x in (279, 282) and z in (247, 250):
                        w.set(x, 67, z, FENCE)
                        w.set(x, 68, z, FENCE)
                w.slab(x, 69, z, "wood")
        # V6b: the roof is all wood slabs (the two planks blocks in it were
        # a dark spawn platform) and a torch on the rim lights the rim walk
        w.torch(281, 67, 247, "floor")

    # ----------------------------------------------------- windmill ---
    def windmill(self):
        """V3 windmill south-east of the lab: a tapered cobble tower (7x7
        base 4 high, 5x5 shaft 6 high, cobble-stair hip cap), a spruce axle
        projecting 2 blocks south at y=74 and four sails, each a fence spar
        with a 2-wide white wool paddle, offset the same way round the hub
        (a pinwheel)."""
        w = self.w
        bx1, bz1, bx2, bz2 = 277, 280, 283, 286
        self.footprints["windmill"] = (bx1, bz1, bx2, bz2)
        self.house_meta["windmill"] = {"fy": 65, "wall_h": 10, "upper": None,
                                       "ridge": "x", "windows": False}
        self.foundation(bx1, bz1, bx2, bz2, 65, COBBLE)
        for x in range(bx1, bx2 + 1):
            for z in range(bz1, bz2 + 1):
                inner = bx1 < x < bx2 and bz1 < z < bz2
                for y in range(66, 70):
                    if not inner:
                        w.set(x, y, z, 48 if H(x, z, y) % 6 == 0 else COBBLE)
                if inner:
                    self.walk.add((x, 66, z))
        # V6b: the shaft and the works are 3 higher (shaft y 70..78, axle
        # y 77), so the lowest sail ends at y 70, 4 clear above the grass
        # (the V6 sail hung at head height one block from the door).
        for x in range(bx1 + 1, bx2):                  # 5x5 shaft
            for z in range(bz1 + 1, bz2):
                edge = x in (bx1 + 1, bx2 - 1) or z in (bz1 + 1, bz2 - 1)
                if not edge:
                    continue
                for y in range(70, 79):
                    corner = x in (bx1 + 1, bx2 - 1) and z in (bz1 + 1, bz2 - 1)
                    w.set(x, y, z, 48 if (corner and y % 2) else COBBLE)
        for x in range(bx1, bx2 + 1):                  # base ledge
            for z in range(bz1, bz2 + 1):
                if x in (bx1, bx2) or z in (bz1, bz2):
                    w.slab(x, 70, z, "cobblestone")
        self.hip(bx1 + 1, bz1 + 1, bx2 - 1, bz2 - 1, 79, material="stone")
        for y in (66, 67):
            w.set(280, y, 286, 0)
        w.door(280, 66, 286, "north")
        w.set(280, 75, 285, 20)
        # V6b: a working mill floor instead of an empty shell with nine
        # glowstone tiles: planks floor, a millstone (2x2 double stone
        # slab), the grain chest, a crate stack by the door, two wall
        # torches.
        for x in range(bx1 + 1, bx2):
            for z in range(bz1 + 1, bz2):
                w.set(x, 65, z, PLANKS)
        for x in (279, 280):
            for z in (282, 283):
                w.slab(x, 66, z, "stone", double=True)
                self.walk.discard((x, 66, z))
        self.chest(282, 66, 281, [(0, WHEAT, 8), (1, SEEDS, 8), (2, BREAD, 1),
                                  (3, BREAD, 1)])
        w.set(278, 66, 285, PLANKS)
        w.set(278, 67, 285, PLANKS)
        w.torch(279, 67, 281, "south")
        w.torch(281, 67, 285, "north")
        # V7 mill and library: cane -> paper -> book -> bookshelf (Beta
        # 1.7.3 recipes: 3 reeds in a row = 3 paper, 3 paper in a column =
        # 1 book, 6 planks + 3 books = 1 bookshelf). A 3-high bookshelf
        # wall along the west side, the recipe signs on its face, the
        # library chest and a crafting table in the south-east corner.
        for z in range(281, 285):
            for y in (66, 67, 68):
                w.set(278, y, z, 47)
            self.walk.discard((278, 66, z))
        self.sign(279, 67, 282, "CRAFT PAPER: / 3 CANE IN / A ROW", "east",
                  wall=True)
        self.sign(279, 67, 283, "STACK 3 PAPER / IN A COLUMN / FOR A BOOK",
                  "east", wall=True)
        self.sign(279, 67, 284, "BUILD A SHELF: / 3 BOOKS AND / 6 PLANKS",
                  "east", wall=True)
        self.chest(282, 66, 284, [(0, REED_ITEM, 9), (1, PAPER, 6),
                                  (2, BOOK, 3), (3, PLANKS, 6)])
        w.set(282, 66, 285, 58)
        hx, hy = 280, 77
        w.log(hx, hy, 286, "spruce")
        w.log(hx, hy, 287, "spruce")
        arms = [((0, 1), (1, 0)), ((1, 0), (0, -1)),
                ((0, -1), (-1, 0)), ((-1, 0), (0, 1))]
        for (ax, ay), (sx, sy) in arms:
            for k in range(1, 8):
                x, y = hx + ax * k, hy + ay * k
                w.set(x, y, 288, FENCE)
                if k >= 3:
                    for j in (1, 2):
                        w.wool(x + sx * j, y + sy * j, 288, "white")
        w.set(hx, hy, 288, 17, 1)
        # V6b: a fence frame along each sail's upper edge and a fence
        # shaft between the axle log and the hub, so no sail or axle top is
        # a bare spawnable ledge in the air
        for x in range(hx - 8, hx + 9):
            for y in range(hy - 8, hy + 9):
                if w.get(x, y, 288)[0] == 35 and w.get(x, y + 1, 288)[0] == 0:
                    w.set(x, y + 1, 288, FENCE)
        w.set(hx, hy, 287, FENCE)
        self.zones["windmill_sails"] = (271, 286, 289, 289)

    # ------------------------------------------------------- mural ---
    # V3 stop colours (gateway wool, road stripe, map dots). Green, blue,
    # grey and yellow-on-sand are the map's ground colours, so stop dots
    # avoid green and blue; hub rooms keep their V2 door colours.
    STOPS = [
        # n, label, leg, gate centre, sign facing (toward the approach)
        (1, "LANDING", "L1", (200, 293), "south", "white"),
        (2, "WOODS", "L1", (200, 283), "south", "lime"),
        (3, "CRAFTING", "L2", (194, 252), "south", "yellow"),
        (4, "TOOLS", "L3", (202, 231), "south", "orange"),
        (5, "MINING", "L4", (218, 222), "west", "cyan"),
        (6, "TORCHES", "L5", (256, 214), "south", "light_blue"),
        (7, "SMELTING", "L6b", (282, 212), "north", "magenta"),
        (8, "FARMING", "L7", (300, 232), "north", "pink"),
        (9, "SHELTER", "L8", (314, 262), "north", "light_gray"),
        (10, "BEDS", "L9", (296, 288), "north", "brown"),
    ]

    # V6 colour key: every numbered place on the island (stops 1..10, hub
    # exits 11..14, the trail end 15): colour, name, one-word icon.
    KEY = [
        (1, "LANDING", "HERE", "white"), (2, "WOODS", "LOGS", "lime"),
        (3, "CRAFTING", "TABLE", "yellow"), (4, "TOOLS", "PICKAXE", "orange"),
        (5, "MINING", "ORE", "cyan"), (6, "TORCHES", "TORCH", "light_blue"),
        (7, "SMELTING", "FURNACE", "magenta"), (8, "FARMING", "WHEAT", "pink"),
        (9, "SHELTER", "DOOR", "light_gray"), (10, "BEDS", "BED", "brown"),
        (11, "MONSTER LAB", "ZOMBIE", "purple"),
        (12, "MINECARTS", "CART", "black"), (13, "REDSTONE", "CLOCK", "red"),
        (14, "PISTON BRIDGE", "PISTON", "blue"),
        (15, "THE END", "LIGHTHOUSE", "gray"),
    ]
    MAP_X0, MAP_Z0 = 212, 294                 # map pixel (0, 0), 17 x 13
    END_SPOT = (178, 300)                     # the lighthouse (trail end)

    def landing_map(self):
        """V6 (replaces the V5 12-high wool map billboard): at the harbour end
        of the trail, beside spawn, the island map lies FLAT under glass in a
        stone-slab frame (sea blue, land green, one dot per
        numbered place in its colour; north is north), and its west edge is
        the colour key: one wool block per place with a sign (numeral, name,
        one-word icon), read from the boardwalk side. The glass keeps the
        display mob-safe (no monster spawns on glass). Map pixels are sampled
        from the built terrain, so the map always matches the island."""
        w = self.w
        X0, Z0 = self.MAP_X0, self.MAP_Z0
        cols, rows = 17, 13
        mx0, mz0, cell = 164, 186, 11
        self.zones["landing"] = (X0 - 4, Z0 - 1, X0 + cols, Z0 + rows)
        self.soft["landing_view"] = (X0 - 6, Z0 - 4, X0 + cols + 3,
                                     Z0 + rows + 5)    # no trees over it

        def pix(i, j):
            water = sand = land = 0
            for a in (2, 5, 8):
                for b in (2, 5, 8):
                    x, zz = mx0 + i * cell + a, mz0 + j * cell + b
                    g = self.ground(x, zz)
                    t = self.bid(x, g + 1, zz)
                    top = self.bid(x, g, zz)
                    if t in (8, 9) or g < 62:
                        water += 1
                    elif top in (12, 24) and g <= 64:
                        sand += 1
                    else:
                        land += 1
            # land / sea only: a yellow beach would hide stop 3's dot
            return "blue" if water >= 5 else "green"
        grid = {(i, j): pix(i, j) for i in range(cols) for j in range(rows)}

        def at(x, zz):
            return (min(cols - 1, max(0, (x - mx0) // cell)),
                    min(rows - 1, max(0, (zz - mz0) // cell)))
        spots = {n: c for (n, label, leg, c, f, col) in self.STOPS}
        spots.update({n: c for (n, lab, col, c, f) in self.HUB_EXITS})
        spots[13] = (256, 244)
        spots[15] = self.END_SPOT
        used = set()
        for (n, name, icon, col) in self.KEY:
            p = at(*spots[n])
            for (di, dj) in ((0, 0), (1, 0), (-1, 0), (0, 1), (0, -1)):
                q = (p[0] + di, p[1] + dj)
                if q not in used and 0 <= q[0] < cols and 0 <= q[1] < rows:
                    break
            used.add(q)
            grid[q] = col
        # walkway (sandstone) west of the frame, the frame, the map
        for x in range(X0 - 4, X0 + cols + 1):
            for z in range(Z0 - 1, Z0 + rows + 1):
                g = self.ground(x, z)
                for y in range(min(g, 63) + 1, 64):
                    w.set(x, y, z, 3)
                for y in range(65, 71):
                    w.set(x, y, z, 0)
                self.reserved.add((x, z))
                if x < X0 - 1:
                    w.set(x, 64, z, 24)
                    self.walk.add((x, 65, z))
                    continue
                inside = X0 <= x < X0 + cols and Z0 <= z < Z0 + rows
                if inside:
                    w.wool(x, 64, z, grid[(x - X0, z - Z0)])
                    w.set(x, 65, z, 20)
                else:
                    w.set(x, 64, z, COBBLE)
                    if x == X0 - 1:
                        continue                         # key column
                    w.slab(x, 65, z, "stone")
        # V8: the map is the lesson (where every numbered stop is), so it
        # stays, with a label; the two jack-o'-lantern corner posts are
        # gone (a plain slab frame) and the label stands on the north-east
        # corner, read from the LS road
        self.post_sign(X0 + cols, 65, Z0 - 1, "ISLAND MAP / DOTS = STOPS / "
                       "KEY: WEST EDGE", "north")
        for k, (n, name, icon, col) in enumerate(self.KEY):
            z = Z0 - 1 + k
            # 2-high key wall: cobble base (glowstone at three spots lights
            # the walkway), the colour block at eye height with its sign
            w.set(X0 - 1, 65, z, 89 if k in (2, 7, 12) else COBBLE)
            w.wool(X0 - 1, 66, z, col)
            self.sign(X0 - 2, 66, z, ["- %d -" % n, name, icon], "west",
                      wall=True)

    # ------------------------------------------------------- ponds ---
    PONDS = [("pond1", 284, 230, 5.5, 4.2), ("pond2", 270, 290, 3.6, 3.0)]

    def ponds(self):
        """Small still ponds (source blocks only, so nothing flows): clay
        bed, sand rim, sugar cane on the rim."""
        w = self.w
        for name, cx, cz, rx, rz in self.PONDS:
            x1, x2 = int(cx - rx - 2), int(cx + rx + 2)
            z1, z2 = int(cz - rz - 2), int(cz + rz + 2)
            self.zones[name] = (x1, z1, x2, z2)
            water = set()
            for x in range(x1, x2 + 1):
                for z in range(z1, z2 + 1):
                    e = ((x - cx) / rx) ** 2 + ((z - cz) / rz) ** 2
                    e += ((H(x, z, 9) % 100) - 50) / 400.0
                    if e <= 1.0:
                        water.add((x, z))
                        w.water_source(x, 65, z)
                        if e < 0.45:
                            w.water_source(x, 64, z)
                            w.set(x, 63, z, 82)
                        else:
                            w.set(x, 64, z, 82 if H(x, z, 4) % 2 else 12)
            for x in range(x1, x2 + 1):
                for z in range(z1, z2 + 1):
                    if (x, z) in water:
                        continue
                    if any((x + a, z + b) in water for a, b in
                           ((1, 0), (-1, 0), (0, 1), (0, -1))):
                        w.set(x, 65, z, 12 if H(x, z, 6) % 3 else 2)
                        if H(x, z, 8) % 4 == 0:
                            w.reed(x, 66, z, 2 + H(x, z, 2) % 2)

    def edging(self):
        """V3 path edges: the cobble kerb is part of the road now, so the
        grass beside it only gets the odd tuft of tall grass or fern."""
        w = self.w
        for (x, z) in self.road_order:
            fy = self.road[(x, z)]
            for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, nz = x + dx, z + dz
                if (nx, nz) in self.road or (nx, nz) in self.reserved:
                    continue
                if self.in_boxes(nx, nz):
                    continue
                if self.bid(nx, fy, nz) != 2 or not self.free_col(nx, nz, fy + 1, 2):
                    continue
                h = H(nx, nz, 11) % 20
                if h < 2:                      # V8: half the path tufts
                    w.tall_grass(nx, fy + 1, nz, "fern" if h == 1 else "grass")
                else:
                    continue
                self.reserved.add((nx, nz))

    # ------------------------------------------------------ relief ---
    def relief(self):
        """Rolling meadows: free grass far from paths and buildings rises
        0-4 blocks on a smooth sine field, tapering to 0 within 3 cells of
        anything authored (every stop, road and lamp stays at y=65)."""
        w = self.w
        boxes = list(self.footprints.values()) + list(self.zones.values()) + \
            list(self.soft.values())
        natural = set()
        for x in range(160, 360):
            for z in range(180, 340):
                if (x, z) in self.reserved or (x, z) in self.road:
                    continue
                if self.ground(x, z) != 65 or self.bid(x, 65, z) != 2:
                    continue
                if not self.free_col(x, z, 66, 8):
                    continue
                if any(x1 - 1 <= x <= x2 + 1 and z1 - 1 <= z <= z2 + 1
                       for (x1, z1, x2, z2) in boxes):
                    continue
                natural.add((x, z))
        dist = {}
        frontier = []
        for (x, z) in sorted(natural):
            if any((x + a, z + b) not in natural for a, b in
                   ((1, 0), (-1, 0), (0, 1), (0, -1))):
                dist[(x, z)] = 1
                frontier.append((x, z))
        d = 1
        while frontier and d < 8:
            nxt = []
            for (x, z) in frontier:
                for a, b in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    p = (x + a, z + b)
                    if p in natural and p not in dist:
                        dist[p] = d + 1
                        nxt.append(p)
            frontier = nxt
            d += 1
        self.relief_cells = 0
        for (x, z) in sorted(natural):
            dd = dist.get((x, z), 8)
            if dd < 3:
                continue
            n = (1.4 + 1.3 * math.sin(x * 0.11 + 1.3) * math.cos(z * 0.09 - 0.4)
                 + 0.9 * math.sin((x + 2 * z) * 0.045 + 0.8)
                 + 0.5 * math.sin(x * 0.27 - z * 0.21))
            h = min(4, max(0, rnd(n * min(1.0, (dd - 2) / 4.0))))
            if h <= 0:
                continue
            for y in range(65, 65 + h):
                w.set(x, y, z, 3)
            w.set(x, 65 + h, z, 2)
            self.relief_cells += 1

    def despeckle(self):
        """V6b: no one-block pimples on the natural ground (a lone grass,
        dirt, stone, sand or gravel block standing 1 above all four
        neighbours reads as a stray block): each is levelled into the ground
        round it. Authored cells (reserved, roads) are never touched."""
        w = self.w
        nat = (1, 2, 3, 12, 13)
        self.despeckled = 0
        for _ in range(4):
            hits = []
            for x in range(160, 369):
                for z in range(180, 337):
                    if (x, z) in self.reserved or (x, z) in self.road:
                        continue
                    g = self.ground(x, z)
                    if self.bid(x, g, z) not in nat or \
                            self.bid(x, g - 1, z) not in nat or \
                            self.bid(x, g + 1, z) not in self.PLAIN:
                        continue
                    if all(self.bid(x + a, g, z + b) in self.PLAIN for (a, b)
                           in ((1, 0), (-1, 0), (0, 1), (0, -1))):
                        hits.append((x, g, z))
            if not hits:
                break
            for (x, g, z) in hits:
                w.set(x, g + 1, z, 0)
                w.set(x, g, z, 0)
                if self.bid(x, g - 1, z) == 3:
                    w.set(x, g - 1, z, 2)
                self.despeckled += 1

    # ================================================================ V3 ===
    # V3 pass (tmp/jobs/tutorial-1021/v3-plan.md). Vertical moves are RIGID
    # column shifts: every block, metadata nibble and tile entity of a
    # column range moves together, so each circuit, gate, sign and chest
    # keeps its exact relative geometry (and so its decompile-cited
    # behaviour); only the absolute y changes. The trackers (walk, growth,
    # signs, chests, spawners, roads, sealed rooms, house storeys) follow.

    def shift_columns(self, cols, dy, ylo, yhi_of=None, restore_top=False,
                      fill=1):
        """Move [ylo, yhi] of every column in `cols` by dy. dy > 0 fills the
        vacated bottom with `fill`; dy < 0 empties the vacated top, or with
        `restore_top` keeps the original top blocks (the natural surface
        stays where it was while what lies under it moves down)."""
        w = self.w
        moved = {}
        for (x, z) in sorted(cols):
            yhi = 127 if yhi_of is None else yhi_of(x, z)
            c = w.chunk(x >> 4, z >> 4)
            base = wl.idx(x & 15, 0, z & 15)
            ob = bytes(c.blocks[base:base + 128])
            om = bytes(c.meta[base:base + 128])
            nb, nm = bytearray(ob), bytearray(om)
            for y in range(ylo, yhi + 1):
                src = y - dy
                if ylo <= src <= yhi:
                    nb[y], nm[y] = ob[src], om[src]
                elif dy > 0:
                    nb[y], nm[y] = fill, 0
                elif restore_top:
                    nb[y], nm[y] = ob[y], om[y]
                else:
                    nb[y], nm[y] = 0, 0
            c.blocks[base:base + 128] = nb
            c.meta[base:base + 128] = nm
            tes = [(k, c.tile_entities.pop(k)) for k in sorted(c.tile_entities)
                   if k[0] == x and k[2] == z and ylo <= k[1] <= yhi]
            for (k, te) in tes:
                ny = k[1] + dy
                if not ylo <= ny <= yhi:
                    continue
                te.value = [(n, wl.Int(ny)) if n == "y" else (n, v)
                            for (n, v) in te.value]
                c.tile_entities[(x, ny, z)] = te
            moved[(x, z)] = (ylo, yhi)
        self._shift_trackers(moved, dy)

    def _shift_trackers(self, moved, dy):
        def mv(p):
            r = moved.get((p[0], p[2]))
            if r and r[0] <= p[1] <= r[1]:
                return (p[0], p[1] + dy, p[2])
            return p
        self.walk = {mv(p) for p in self.walk}
        self.growth = {mv(p) for p in self.growth}
        self.signs = [(mv(p), ln) for (p, ln) in self.signs]
        self.chests = [(mv(p), it) for (p, it) in self.chests]
        self.spawners = [mv(p) for p in self.spawners]
        self.no_window = {mv(p) for p in self.no_window}
        for k, fy in list(self.road.items()):
            r = moved.get(k)
            if r and r[0] <= fy <= r[1]:
                self.road[k] = fy + dy
        for i, box in enumerate(self.sealed):
            x1, z1, x2, z2, y1, y2 = box[:6]
            r = moved.get((x1, z1))
            if r and (x2, z2) in moved and r[0] <= y1 <= r[1]:
                self.sealed[i] = ((x1, z1, x2, z2, y1 + dy, y2 + dy)
                                  + tuple(box[6:]))
        for name, meta in self.house_meta.items():
            x1, z1, x2, z2 = self.footprints[name]
            if (x1, z1) in moved and (x2, z2) in moved:
                meta["fy"] += dy
                ub = self.upper_boxes.get(name)
                if ub:
                    self.upper_boxes[name] = ub[:4] + (ub[4] + dy,)

    def lower_mine(self):
        """'Downhill to the mine': the whole stop-5 complex (quarry, band
        face, entrance tunnel, hut, the spiral down to the gallery, the
        ladder shaft) drops 3 blocks as one rigid body. Quarry/hut columns
        move whole; the face, tunnel and spiral columns move only below
        their natural surface, so the hill stays where it was. Ore depths
        stay truthful (coal 67-68, iron 65-66 and 38-39, gold/lapis/
        redstone/diamond at 10-11)."""
        w = self.w
        d = self.mine_dy = -3
        for y in range(1, 128):                    # ladder: re-run later
            if self.bid(232, y, 207) == 65:
                w.set(232, y, 207, 1)
        full = {(x, z) for x in range(222, 241) for z in range(216, 232)}
        self.shift_columns(full, d, 4)
        seg = {(x, z) for x in range(222, 241) for z in range(213, 216)}
        seg |= {(x, z) for x in range(225, 238) for z in range(199, 213)}
        gtop = {p: self.ground(*p) for p in seg}
        self.shift_columns(seg, d, 4, lambda x, z: gtop[(x, z)],
                           restore_top=True)
        for x in range(222, 241):                  # face: stone to the top
            t = gtop[(x, 215)]
            for y in range(t - 4, t):
                if self.bid(x, y, 215) in (2, 3):
                    w.set(x, y, 215, 1)
        # pit walls: bare stone where the surrounding ground stands higher
        for (x, z) in sorted(full):
            for (a, b) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                q = (x + a, z + b)
                if q in full or q in seg or q in self.road:
                    continue
                gp, gq = self.ground(x, z), self.ground(*q)
                for y in range(gp + 1, gq):
                    if self.bid(q[0], y, q[1]) == 3:
                        w.set(q[0], y, q[1], 1)

    def mine_reladder(self):
        """Gallery ladder after the lowering and the crest: from the
        gallery floor to the crest surface beside the wordmark."""
        top = self.ground(232, 208)
        for y in range(13 + self.mine_dy, top + 1):
            self.w.ladder(232, y, 207, "north")

    # name, (x1, z1, x2, z2), lift. The castle (keep, plaza, lab) tops a
    # 6-block hill; the cart yard, the town and three grass terraces form a
    # 3-block shoulder round it. Gaps between tiers are cobble-stair ramps.
    HILL = [("castle", (244, 226, 268, 284), 6),
            ("yard", (206, 246, 240, 266), 3),
            ("west_n", (241, 246, 243, 254), 3),
            ("west_s", (241, 258, 243, 266), 3),
            ("sw", (239, 267, 243, 284), 3),
            ("town", (269, 244, 293, 267), 3),
            ("south", (248, 285, 263, 293), 3)]

    def lift_hill(self):
        self.hub_dy = 6
        for name, (x1, z1, x2, z2), dy in self.HILL:
            cols = {(x, z) for x in range(x1, x2 + 1)
                    for z in range(z1, z2 + 1)}
            self.shift_columns(cols, dy, 60)
            self.zones["hill_" + name] = (x1, z1, x2, z2)

    def station_eave(self):
        """V6b: the west station's gable overhang column (x 205) lies
        outside the lifted yard box, so lift_hill() left it 3 below its
        roof. Move it up to the roof (y 72..74) and put a floor torch on
        the ridge (it was a dark spawn strip over the station)."""
        w = self.w
        dy = 3
        for z in range(254, 259):
            col = [(y, w.get(205, y, z)) for y in range(69, 72)
                   if w.get(205, y, z)[0] in (53, PLANKS)]
            for (y, _) in col:
                w.set(205, y, z, 0)
            for (y, (b, m)) in col:
                w.set(205, y + dy, z, b, m)
        w.torch(208, 75, 256, "floor")

    def ramp(self, steps, ascends):
        """Cobble-stair ramp: steps = [(row of (x, z) across the road, stair
        y)]. Fills under each stair, clears 4 above, records the road cell
        at the stair (walk cell above it), and raises a cobble side wall on
        each flank cell that lies lower than the stair."""
        w = self.w
        for row, y in steps:
            for (x, z) in row:
                g = self.ground(x, z)
                for yy in range(g + 1, y):
                    w.set(x, yy, z, COBBLE)
                w.stairs(x, y, z, ascends, "cobblestone")
                for yy in range(y + 1, y + 5):
                    w.set(x, yy, z, 0)
                self.road[(x, z)] = y
                self.reserved.add((x, z))
            (ax, az), (bx, bz) = row[0], row[-1]
            px, pz = (bx > ax) - (bx < ax), (bz > az) - (bz < az)
            for (fx, fz) in ((ax - px, az - pz), (bx + px, bz + pz)):
                if (fx, fz) in self.road:
                    continue
                g = self.ground(fx, fz)
                for yy in range(g + 1, y + 1):
                    w.set(fx, yy, fz, 48 if H(fx, fz, yy) % 5 == 0 else COBBLE)
                # V6b: a cobble-slab coping on a built flank wall (its bare
                # top was a dark spawn ledge beside the ramp)
                if g < y and self.bid(fx, y + 1, fz) == 0:
                    w.slab(fx, y + 1, fz, "cobblestone")
                self.reserved.add((fx, fz))

    def ramps(self):
        def across_z(x, y):
            return ([(x, 221), (x, 222), (x, 223)], y)
        self.ramp([across_z(219, 64), across_z(220, 63), across_z(221, 62)],
                  "west")                                   # L4 into the pit
        self.ramp([([(x, 223), (x, 224), (x, 225)], y)
                   for (x, y) in ((241, 63), (242, 64), (243, 65))], "east")
        self.ramp([([(x, 255), (x, 256), (x, 257)], y)
                   for (x, y) in ((241, 69), (242, 70), (243, 71))], "east")
        self.ramp([([(x, 255), (x, 256), (x, 257)], y)
                   for (x, y) in ((269, 70), (270, 69))], "west")
        self.ramp([([(x, 255), (x, 256), (x, 257)], y)
                   for (x, y) in ((294, 67), (295, 66), (296, 65))], "west")
        self.ramp([([(244, z), (245, z), (246, z)], y)
                   for (z, y) in ((288, 66), (287, 67), (286, 68), (285, 69),
                                  (284, 70), (283, 71))], "north")

    def retain(self):
        """Cobble (mossy-flecked) retaining walls on every exposed side of
        the lifted tiers: the castle stands on a cobble plinth and each
        terrace on a cobble wall. Only natural stone/dirt is replaced."""
        w = self.w
        for name, (x1, z1, x2, z2), dy in self.HILL:
            floor = 65 + dy
            for x in range(x1, x2 + 1):
                for z in range(z1, z2 + 1):
                    if x1 < x < x2 and z1 < z < z2:
                        continue
                    outs = [self.ground(x + a, z + b) for (a, b) in
                            ((1, 0), (-1, 0), (0, 1), (0, -1))
                            if not (x1 <= x + a <= x2 and z1 <= z + b <= z2)]
                    if not outs:
                        continue
                    for y in range(min(outs) + 1, floor):
                        if self.bid(x, y, z) in (1, 3):
                            w.set(x, y, z, 48 if H(x, z, y) % 5 == 0
                                  else COBBLE)

    def quarry_benches(self):
        """Quarry 8-10 deep: 2-high stone benches stepping up away from the
        pit on the west and east sides (only raising natural ground), with
        exposed coal (any height) and iron (only at y <= 63, so the 'IRON:
        UNDER 64' lesson stays true); a ladderway of 2-high ladders up the
        east risers; the rim above the face kept free of greenery."""
        w = self.w
        self.soft["quarry_rim"] = (212, 207, 248, 222)

        def bench(x, z, top):
            if (x, z) in self.reserved or (x, z) in self.road:
                return
            if self.in_boxes(x, z):
                return
            g = self.ground(x, z)
            if g >= top or self.bid(x, g, z) not in (1, 2, 3):
                return
            if not self.free_col(x, z, g + 1, top - g + 1):
                return
            for y in range(g, top + 1):
                h = H(x, z, y)
                if h % 11 == 0:
                    b = 16
                elif y <= 63 and h % 13 == 0:
                    b = 15
                else:
                    b = 1
                w.set(x, y, z, b)
        for k in range(8):
            top = 62 + 2 * (1 + k // 2)
            for z in range(210, 223):
                bench(239 + k, z, top)
            for z in range(210, 220):
                bench(221 - k, z, top)
        for k in (0, 2, 4, 6):
            xf, top = 239 + k, 62 + 2 * (1 + k // 2)
            for y in (top - 1, top):
                if solid(self.bid(xf, y, 219)) and self.bid(xf - 1, y, 219) == 0:
                    w.ladder(xf - 1, y, 219, "west")

    QUARRY_BOX = (222, 216, 238, 222)

    def road_style(self):
        """V6 road: the 3-wide spine is sandstone (gravel is used only on the
        quarry floor), with a flush cobble kerb on each side wherever the
        neighbouring cell is plain ground at road height. Plank boardwalk,
        stairs, gates and ditches are left as they are. Kerb cells are
        walkable, so they join self.walk."""
        w = self.w
        qx1, qz1, qx2, qz2 = self.QUARRY_BOX
        for (x, z), fy in sorted(self.road.items()):
            if self.bid(x, fy, z) != GRAVEL:
                continue
            if qx1 <= x <= qx2 and qz1 <= z <= qz2:
                continue
            w.set(x, fy, z, 24)
        legs = set()
        for name, pts, surface in self.LEGS:
            if surface != "planks":
                legs.update(self.leg_cells(pts))
        for (x, z) in sorted(legs):
            fy = self.road.get((x, z))
            if fy is None or self.bid(x, fy, z) != 24:
                continue
            for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, nz = x + dx, z + dz
                if (nx, nz) in self.road or (nx, nz) in self.kerb:
                    continue
                if (nx, nz) in self.reserved or any(
                        x1 <= nx <= x2 and z1 <= nz <= z2 for name, (
                            x1, z1, x2, z2) in list(self.footprints.items())
                        + list(self.zones.items())
                        if not name.startswith("hill_")):
                    continue
                if self.ground(nx, nz) != fy or \
                        self.bid(nx, fy, nz) not in (2, 3, 12, 13):
                    continue
                if not self.free_col(nx, nz, fy + 1, 3):
                    continue
                w.set(nx, fy, nz, COBBLE)
                self.kerb[(nx, nz)] = fy
                self.walk.add((nx, fy + 1, nz))
                self.reserved.add((nx, nz))

    # V6 big numerals: 3 wide x 5 tall wool digits.
    DIGITS = {
        "0": ["###", "#.#", "#.#", "#.#", "###"],
        "1": [".#.", "##.", ".#.", ".#.", "###"],
        "2": ["###", "..#", "###", "#..", "###"],
        "3": ["###", "..#", ".##", "..#", "###"],
        "4": ["#.#", "#.#", "###", "..#", "..#"],
        "5": ["###", "#..", "###", "..#", "###"],
        "6": ["###", "#..", "###", "#.#", "###"],
        "7": ["###", "..#", ".#.", ".#.", ".#."],
        "8": ["###", "#.#", "###", "#.#", "###"],
        "9": ["###", "#.#", "###", "..#", "###"],
    }
    LIGHT_COLOURS = ("white", "yellow", "light_gray")

    @staticmethod
    def right_of(facing):
        """Unit (x, z) step to the right of a viewer who stands on the
        `facing` side of a board and looks at it."""
        dx, dz = wl.DIRS[facing]
        return (dz, -dx)

    def numeral_board(self, cx, cz, y0, facing, text, col, flush=False,
                      back_layer=True):
        """Wool numeral board in the plane through (cx, cz) perpendicular to
        `facing` (the side the viewer approaches from): the digits (3x5, in
        the stop colour) on a back panel with a 1-block margin, rows y0 ..
        y0+6. The panel is white, or black for the light stop colours
        (white / yellow / light grey would vanish on white). Unless `flush`
        (set into a wall), a second layer behind carries the same numeral
        the right way round for a viewer on the far side, and a stone slab
        row caps the board. Returns the board's front cells."""
        w = self.w
        # V8: one style everywhere: black digits on a white board (the stop
        # colour stays on the gate lintel); the back layer is plain white
        back, ink = "white", "black"
        rx, rz = self.right_of(facing)
        dx, dz = wl.DIRS[facing]
        width = 4 * len(text) + 1
        u0 = -(width // 2)

        def colour_at(k, r):
            di, dc = (k - 1) // 4, (k - 1) % 4
            if 1 <= r <= 5 and k >= 1 and dc < 3 and di < len(text):
                if self.DIGITS[text[di]][r - 1][dc] == "#":
                    return ink
            return back
        cells = []
        for k in range(width):
            u = u0 + k
            x, z = cx + rx * u, cz + rz * u
            for r in range(7):
                y = y0 + 6 - r
                w.wool(x, y, z, colour_at(k, r))
                cells.append((x, y, z))
                if not flush and back_layer:
                    w.wool(x - dx, y, z - dz, back)
            if not flush:
                w.slab(x, y0 + 7, z, "stone")
                if back_layer:
                    w.slab(x - dx, y0 + 7, z - dz, "stone")
        return cells

    BOARD_CLEAR = (0, 31, 37, 38)

    def side_board(self, cx, cz, fy, facing, text, half, clear=(), first=1):
        """V8 numbered board BESIDE a path entrance (not on the lintel):
        the white board with black digits (7 rows, fy+1..fy+7: 3x5 digits
        with a 1-block margin), a stone slab cap at fy+8, a plain white
        back layer (half the height of the V6 lintel boards, top fy+12); it stands in
        the gate line just outside a post and faces the approach. Tries the
        approaching player's right side, then the left; columns must be off
        the road and kerbs, on ground no more than 3 below the path (cobble
        fill under it), with free air (or a block id in `clear`) up to the
        cap. Returns True when placed."""
        w = self.w
        dx, dz = wl.DIRS[facing]
        rx, rz = self.right_of(facing)
        width = 4 * len(text) + 1
        ok_ids = set(self.BOARD_CLEAR) | set(clear)
        # with its plain back layer first; a single-layer board only where
        # the cell behind is taken (e.g. the lab's corbel band behind exit 11)
        for (s, back) in ((first, True), (-first, True), (first, False),
                          (-first, False)):
            cols = []
            good = True
            for k in range(width):
                u = s * (half + 1 + k)
                layers = [(cx + rx * u, cz + rz * u)]
                if back:
                    layers.append((cx + rx * u - dx, cz + rz * u - dz))
                for (x, z) in layers:
                    if (x, z) in self.road or (x, z) in self.kerb or \
                            (x, fy + 1, z) in self.walk:
                        good = False
                        break
                    g = self.ground(x, z)
                    if g > fy and not all(self.bid(x, y, z) in ok_ids
                                          for y in range(fy + 1, g + 1)):
                        good = False
                        break
                    if g < fy - 3 or not all(
                            self.bid(x, y, z) in ok_ids
                            for y in range(fy + 1, fy + 9)):
                        good = False
                        break
                    cols.append((x, z))
                if not good:
                    break
            if not good:
                continue
            for (x, z) in cols:
                g = self.ground(x, z)
                for y in range(min(g, fy) + 1, fy + 1):
                    w.set(x, y, z, COBBLE)
                for y in range(fy + 1, fy + 9):
                    w.set(x, y, z, 0)
                self.reserved.add((x, z))
                self.walk.discard((x, fy + 1, z))
            mid = s * (half + 1 + width // 2)
            self.numeral_board(cx + rx * mid, cz + rz * mid, fy + 1, facing,
                               text, "black", back_layer=back)
            return True
        return False

    def gate_frame(self, cx, cz, fy, facing, n, col, label, half=2,
                   post=("log", "oak"), torches=True, back_layer=True,
                   label_on_post=False, board_clear=(), board_side=1):
        """V6 numbered gateway: posts `half` cells either side of the centre
        line, a plank lintel with the colour wool over each post, the
        numeral board on the lintel facing the approach, a 'STOP n / NAME'
        sign on the lintel's approach face and a wall torch on each post's
        approach face (light at the entrance)."""
        w = self.w
        dx, dz = wl.DIRS[facing]
        rx, rz = self.right_of(facing)
        span = [(cx + rx * k, cz + rz * k) for k in range(-half, half + 1)]
        posts = (span[0], span[-1])
        for (px, pz) in posts:
            g = self.ground(px, pz)
            for y in range(min(g, fy) + 1, fy + 1):
                w.set(px, y, pz, COBBLE)
            for y in range(fy + 1, fy + 4):
                if post[0] == "log":
                    w.log(px, y, pz, post[1])
                else:
                    w.set(px, y, pz, post[1])
            if torches:
                w.torch(px + dx, fy + 3, pz + dz, facing)
            self.reserved.add((px, pz))
            self.walk.discard((px, fy + 1, pz))
        for (sx, sz) in span:
            if (sx, sz) in posts:
                w.wool(sx, fy + 4, sz, col)
            else:
                w.set(sx, fy + 4, sz, PLANKS)
        # V8: the numeral board stands beside the entrance at eye height;
        # only where neither side has room does it go back on the lintel
        if not self.side_board(cx, cz, fy, facing, str(n), half,
                               clear=board_clear, first=board_side):
            self.board_on_lintel.append(n)
            self.numeral_board(cx, cz, fy + 5, facing, str(n), col,
                               back_layer=back_layer)
        if label and label_on_post:
            # at eye height on the left post's approach face (an awning
            # covers the lintel's face)
            px, pz = posts[0]
            self.sign(px + dx, fy + 2, pz + dz, label, facing, wall=True)
        elif label:
            self.sign(cx + dx, fy + 4, cz + dz, label, facing, wall=True)

    def gateways(self):
        """Numbered gateway at every stop (V6): log posts, plank lintel, a
        the stop colour on the lintel over each post, the 'STOP n / NAME'
        sign and torches on the posts; V8: the numeral board (black on
        white) stands beside the entrance (side_board) and the coloured
        road stripes are gone."""
        w = self.w
        for (n, label, leg, (cx, cz), facing, col) in self.STOPS:
            fy = self.road[(cx, cz)]
            dx, dz = wl.DIRS[facing]
            rx, rz = self.right_of(facing)
            # V6b: gate 7's board abuts the smelting shed's eave; a single
            # plane keeps the gate, awning and shed from reading as one lump
            self.gate_frame(cx, cz, fy, facing, n, col,
                            ["STOP %d" % n, label], back_layer=(n != 7))
            if n == 7:
                # V8: with the board off the lintel, gate 7's lintel ends
                # against the smelting shed's eave stair; a second plank
                # course makes it a header that the eave dies into (as the
                # V6 board did), not a stair butting a bare beam
                for k in range(-2, 3):
                    x, z = cx + rx * k, cz + rz * k
                    if self.bid(x, fy + 5, z) == 0:
                        w.set(x, fy + 5, z, PLANKS)
            # V8: no coloured threshold stripe across the road (one plain
            # path surface the eye can follow; the colour stays on the
            # lintel over each post)

    # 5x5 arrow (tip row first) inlaid in the road and kerbs: '#' dark
    # wool, 'G' glowstone: a light bar across the tail (centre + both kerbs)
    # lights the road and kerbs up to the next arrow.
    ARROW = ["..#..", ".###.", "#####", "..#..", "G.G.G"]

    def arrow_cells(self, c, d):
        """(cell, char) of the 5x5 arrow centred on road cell c pointing d."""
        px, pz = d[1], d[0]
        out = []
        for r, row in enumerate(self.ARROW):
            t = 2 - r
            for k, ch in enumerate(row):
                if ch == ".":
                    continue
                u = k - 2
                out.append(((c[0] + d[0] * t + px * u, c[1] + d[1] * t + pz * u),
                            ch))
        return out

    def road_glow(self, x, y, z):
        """V6b: a road light (glowstone flush in the road) is always framed
        by road or kerb: a grass / dirt / sand neighbour at its level
        becomes a flush cobble kerb cell (a light set into turf reads as a
        stray tile)."""
        w = self.w
        w.set(x, y, z, 89)
        for (a, b) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if self.bid(x + a, y, z + b) in (2, 3, 12):
                up = self.bid(x + a, y + 1, z + b)
                if up in (31, 37, 38):
                    w.set(x + a, y + 1, z + b, 0)
                elif up not in (0, COBBLE, 24):
                    continue            # a sapling / sign keeps its soil
                w.set(x + a, y, z + b, COBBLE)

    def stripes_and_arrows(self):
        """A 1-wide wool stripe in the stop colour on the centre line for
        the road cells before each gateway; and V6 5x5 dark wool arrows
        about every 11 cells on every sandstone leg, pointing the way the
        trail runs, each with a glowstone tip and tail (the road and kerb
        cells round an arrow stay within light 8 of one)."""
        w = self.w
        stripe = set()
        for (n, label, leg, c, facing, col) in self.STOPS:
            line = self.leg_centre[leg]
            k = line.index(c)
            run = line[max(0, k - 9):max(0, k - 1)]
            for i, (x, z) in enumerate(run):
                fy = self.road.get((x, z))
                if fy is None or self.bid(x, fy, z) not in (24, PLANKS):
                    continue
                if self.free_col(x, z, fy + 1, 2):
                    # the stripe's far end is a glowstone (it lights the
                    # kerbs there; the gate torches light the near end)
                    # V8: only the light stays; the coloured centre
                    # stripe is gone (one plain path, muted palette)
                    if i == 0 and len(run) == 8:
                        self.road_glow(x, fy, z)
                    stripe.add((x, z))
        # junction lights: a glowstone in the road at every bend
        for name, pts, surface in self.LEGS:
            if surface != "gravel":
                continue
            for (x, z) in pts[1:-1] + [pts[0], pts[-1]]:
                fy = self.road.get((x, z))
                if fy is None or (x, z) in stripe:
                    continue
                if self.bid(x, fy, z) != 24 or \
                        not self.free_col(x, z, fy + 1, 2):
                    continue
                self.road_glow(x, fy, z)
                stripe.add((x, z))
        gates = [s[3] for s in self.STOPS]
        self.n_arrows = 0
        for name, pts, surface in self.LEGS:
            if surface != "gravel":
                continue
            line = self.leg_centre[name]
            k = 6
            while k < len(line) - 3:
                placed = False
                for kk in (k, k + 1, k - 1, k + 2, k + 3):
                    if not 2 <= kk < len(line) - 2:
                        continue
                    c = line[kk]
                    d = (line[kk + 1][0] - c[0], line[kk + 1][1] - c[1])
                    if any(line[kk + j] != (c[0] + d[0] * j, c[1] + d[1] * j)
                           for j in (-2, -1, 1, 2)):
                        continue
                    fy = self.road.get(c)
                    cells = self.arrow_cells(c, d)
                    ok = True
                    for (p, ch) in cells:
                        if self.road.get(p, self.kerb.get(p)) != fy or \
                                self.bid(p[0], fy, p[1]) not in (24, COBBLE) \
                                or not self.free_col(p[0], p[1], fy + 1, 2) \
                                or p in stripe or any(
                                    abs(p[0] - g[0]) + abs(p[1] - g[1]) <= 3
                                    for g in gates):
                            ok = False
                            break
                    if not ok:
                        continue
                    for (p, ch) in cells:
                        if ch == "G":
                            self.road_glow(p[0], fy, p[1])
                        else:
                            w.wool(p[0], fy, p[1], "gray")
                    self.n_arrows += 1
                    placed = True
                    k = kk + 11
                    break
                if not placed:
                    k += 4

    def trail_end(self):
        """V6 trail end (15): from the quay's west end a plank bridge crosses
        the inlet to the headland, cobble stairs climb it, and a sandstone
        patio below the lighthouse (glowstone beacon on its cap) carries
        the 'done' arch: a numbered gate (15, grey) with its sign, facing
        the arrival."""
        w = self.w
        self.zones["end_path"] = (180, 302, 193, 308)
        for x in (191, 192, 193):                  # bridge over the inlet
            for z in (304, 305, 306):
                w.set(x, 64, z, PLANKS)
                for y in range(65, 70):
                    w.set(x, y, z, 0)
                self.road[(x, z)] = 64
                self.reserved.add((x, z))
            for z in (303, 307):                   # rails on log piles
                if self.bid(x, 64, z) not in (0, 8, 9):
                    continue
                y = 63
                while y > 52 and self.bid(x, y, z) in (0, 8, 9):
                    w.log(x, y, z, "spruce")
                    y -= 1
                w.log(x, 64, z, "spruce")
                w.set(x, 65, z, FENCE)
                self.reserved.add((x, z))
        self.ramp([([(x, 304), (x, 305), (x, 306)], y)
                   for (x, y) in ((190, 65), (189, 66), (188, 67), (187, 68),
                                  (186, 69), (185, 70))], "west")
        for x in range(181, 185):                  # patio below the tower
            for z in range(303, 308):
                g = self.ground(x, z)
                for y in range(min(g, 70) + 1, 70):
                    w.set(x, y, z, COBBLE)
                w.set(x, 70, z, 24)
                for y in range(71, 76):
                    w.set(x, y, z, 0)
                self.road[(x, z)] = 70
                self.reserved.add((x, z))
        self.gate_frame(184, 305, 70, "east", 15, "gray",
                        ["15 THE END", "WELL DONE!"])
        self.post_sign(182, 71, 305, "TRAIL DONE! / NOW YOU KNOW / THE "
                       "BASICS. GO / BUILD A WORLD!", "east")
        self.road.pop((182, 305), None)

    FORECOURTS = [(193, 244, 197, 248), (199, 220, 203, 224),
                  (228, 222, 232, 226), (254, 203, 258, 207),
                  (286, 212, 290, 216), (299, 238, 303, 242),
                  (312, 264, 316, 268), (294, 290, 298, 294)]

    def forecourts(self):
        """A plain 5x5 stone forecourt in front of each lesson entrance;
        decoration keeps out (soft box), existing stop objects stay."""
        w = self.w
        for i, (x1, z1, x2, z2) in enumerate(self.FORECOURTS):
            self.soft["forecourt%d" % i] = (x1, z1, x2, z2)
            for x in range(x1, x2 + 1):
                for z in range(z1, z2 + 1):
                    if (x, z) in self.road or (x, z) in self.reserved:
                        continue
                    g = self.ground(x, z)
                    if self.bid(x, g, z) != 2:
                        continue
                    if not self.free_col(x, z, g + 1, 3):
                        continue
                    w.set(x, g, z, 1)
                    self.walk.add((x, g + 1, z))
                    self.reserved.add((x, z))

    # ---------------------------------------------------- windows ---
    ATTACH = {23, 25, 26, 50, 54, 55, 58, 61, 62, 63, 64, 65, 68, 69, 71, 75,
              76, 77, 93, 94, 96}
    WINDOWABLE = {PLANKS, COBBLE, 24, 45, 35, 48, 43}

    def window_ok(self, x, y, z):
        if (x, y, z) in self.no_window:
            return False
        if self.bid(x, y, z) not in self.WINDOWABLE:
            return False
        return not any(self.bid(x + a, y + b, z + c) in self.ATTACH
                       for (a, b, c) in ((1, 0, 0), (-1, 0, 0), (0, 1, 0),
                                         (0, -1, 0), (0, 0, 1), (0, 0, -1)))

    def windows(self):
        """Glass windows in the house walls (never on a cell that holds or
        touches a torch, sign, door, bed, chest, table, furnace or redstone
        part) and 2-wide windows between the upper-storey posts."""
        w = self.w
        skip = {"lab", "tunnel", "s2_nook"}
        self.n_windows = 0
        for name, meta in self.house_meta.items():
            if name in skip or not meta["windows"]:
                continue
            x1, z1, x2, z2 = self.footprints[name]
            fy = meta["fy"]
            ys = (fy + 2,) if meta["wall_h"] < 4 else (fy + 2, fy + 3)
            for x in range(x1, x2 + 1):
                for z in range(z1, z2 + 1):
                    if not (x in (x1, x2) or z in (z1, z2)):
                        continue
                    if x in (x1, x2) and z in (z1, z2):
                        continue
                    along = (x - x1) if z in (z1, z2) else (z - z1)
                    n = (x2 - x1) if z in (z1, z2) else (z2 - z1)
                    if along < 2 or along > n - 2 or along % 2:
                        continue
                    if self.bid(x, fy + 1, z) == 0:
                        continue
                    if all(self.window_ok(x, y, z) for y in ys):
                        for y in ys:
                            w.set(x, y, z, 20)
                        self.n_windows += 1
            ub = self.upper_boxes.get(name)
            if ub:
                ux1, uz1, ux2, uz2, yb = ub
                for x in range(ux1, ux2 + 1):
                    for z in range(uz1, uz2 + 1):
                        if not (x in (ux1, ux2) or z in (uz1, uz2)):
                            continue
                        # one window per gable wall (never a pair of eyes)
                        along = (x - ux1) if z in (uz1, uz2) else (z - uz1)
                        n = (ux2 - ux1) if z in (uz1, uz2) else (uz2 - uz1)
                        if along != n // 2:
                            continue
                        if self.bid(x, yb + 2, z) != 17 and \
                                self.window_ok(x, yb + 2, z):
                            w.set(x, yb + 2, z, 20)
                            self.n_windows += 1

    def light_sealed(self):
        """Closed rooms and roof decks (attics, turret tops, letter tops,
        lantern gallery) get no dark spawn spot: glowstone in the floor
        below a dark cell ('glow') or a floor torch on it ('torch')."""
        w = self.w
        fixed = 0
        for _ in range(6):
            todo = []
            for box in self.sealed:
                x1, z1, x2, z2, y1, y2 = box[:6]
                mode = box[6] if len(box) > 6 else "glow"
                for (x, y, z) in w.dark_spawn_spots(x1, z1, x2, z2):
                    if y1 <= y <= y2:
                        todo.append((x, y, z, mode))
            if not todo:
                break
            done = []
            for (x, y, z, mode) in todo:
                if any(abs(x - a) + abs(z - c) + abs(y - b) <= 4
                       for (a, b, c) in done):
                    continue
                if mode == "glow" and self.bid(x, y - 1, z) in \
                        (PLANKS, COBBLE, 48, 43, 35, 24, 45):
                    w.set(x, y - 1, z, 89)
                else:
                    w.torch(x, y, z, "floor")
                done.append((x, y, z))
                fixed += 1
            w.compute_light()
        self.sealed_fixes = fixed

    # ------------------------------------------------- 1.4/1.5 decoration ---
    TREES = ([((190, 268), "oak"), ((214, 262), "oak"), ((184, 252), "oak"),
              ((236, 286), "oak"), ((262, 296), "oak"), ((280, 312), "oak"),
              ((300, 306), "oak"), ((330, 262), "oak"), ((326, 282), "oak"),
              ((334, 240), "oak"), ((246, 196), "oak"), ((276, 196), "oak"),
              ((300, 226), "oak")]
             + [((p), "birch") for p in ((192, 294), (212, 308), (226, 314),
                                         (176, 262), (182, 236))]
             + [((p), "spruce") for p in ((212, 196), (248, 222), (252, 198),
                                          (208, 230))]
             + [((350, 206), "oak"), ((338, 218), "oak")])

    def near_reserved(self, x, z, r):
        for dx in range(-r, r + 1):
            for dz in range(-r, r + 1):
                if (x + dx, z + dz) in self.reserved:
                    return True
        return False

    def boxes(self):
        return (list(self.footprints.values()) + list(self.zones.values())
                + list(self.soft.values()))

    def floor_glow(self, x1, z1, x2, z2, y, step=6, avoid=()):
        """Glowstone set flush into a floor (y) on a step x step lattice
        centred in the box; skips cells under anything but air and cells
        within 1 of a block id in `avoid` (redstone parts). Every feet cell
        is then within Manhattan 6 of a glowstone: 15 - 6 - 1 = 8."""
        w = self.w
        ox = x1 + ((x2 - x1) % step) // 2
        oz = z1 + ((z2 - z1) % step) // 2
        for x in range(ox, x2 + 1, step):
            for z in range(oz, z2 + 1, step):
                if w.get(x, y + 1, z)[0] != 0 or w.get(x, y + 2, z)[0] != 0:
                    continue
                if w.get(x, y, z)[0] in (0, 8, 9):
                    continue          # V6: never fill a hole or a pond
                if any(w.get(x + dx, yy, z + dz)[0] in avoid
                       for dx in (-1, 0, 1) for dz in (-1, 0, 1)
                       for yy in (y, y + 1)):
                    continue
                w.set(x, y, z, 89)
                self.n_floor_glow += 1

    def tree_v2(self, x, z, kind):
        """Beta-style trees: 'oak' / 'birch' (the 2.0 blob, birch one log
        taller), 'spruce' (stacked cone) and 'bigoak' (7-log trunk, four
        branch logs, round crown). Every leaf is placed only if it is within
        Manhattan 4 of a log of this tree (BlockLeaves.java:56 decay radius);
        check_leaves() re-verifies the whole world."""
        w = self.w
        fy = self.ground(x, z)
        y0 = fy + 1
        logs = []
        leaves = set()
        wood = {"bigoak": "oak"}.get(kind, kind)
        if kind in ("oak", "birch"):
            h = 5 + (1 if kind == "birch" else 0) + (H(x, z, 31) % 2)
            logs = [(x, y0 + i, z) for i in range(h)]
            top = y0 + h - 1
            for (y, r) in ((top - 2, 2), (top - 1, 2), (top, 1), (top + 1, 1)):
                for dx in range(-r, r + 1):
                    for dz in range(-r, r + 1):
                        if abs(dx) == r and abs(dz) == r and (r == 2 or y == top + 1):
                            if H(x + dx, z + dz, y) % 2 or y == top + 1:
                                continue
                        leaves.add((x + dx, y, z + dz))
        elif kind == "spruce":
            h = 7 + H(x, z, 32) % 2
            logs = [(x, y0 + i, z) for i in range(h)]
            top = y0 + h - 1
            leaves.add((x, top + 1, z))
            radii = [1, 1, 2, 1, 2, 1, 2]
            for k, r in enumerate(radii):
                y = top - k
                if y < y0 + 1:
                    break
                for dx in range(-r, r + 1):
                    for dz in range(-r, r + 1):
                        if abs(dx) == r and abs(dz) == r and r >= 1:
                            if r == 1 and k == 0:
                                continue
                            if r == 2:
                                continue
                        leaves.add((x + dx, y, z + dz))
        else:  # bigoak
            h = 7
            logs = [(x, y0 + i, z) for i in range(h)]
            top = y0 + h - 1
            for (dx, dz) in ((2, 0), (-2, 0), (0, 2), (0, -2)):
                logs.append((x + dx // 2, top - 2, z + dz // 2))
                logs.append((x + dx, top - 1, z + dz))
            cy = top
            for dx in range(-3, 4):
                for dz in range(-3, 4):
                    for dy in range(-2, 3):
                        e = (dx / 3.4) ** 2 + (dz / 3.4) ** 2 + (dy / 2.4) ** 2
                        if e <= 1.0:
                            leaves.add((x + dx, cy + dy, z + dz))
        for (lx, ly, lz) in logs:
            w.log(lx, ly, lz, wood)
        logset = set(logs)
        for (lx, ly, lz) in sorted(leaves):
            if (lx, ly, lz) in logset:
                continue
            if min(abs(lx - a) + abs(ly - b) + abs(lz - c)
                   for (a, b, c) in logs) > 4:
                continue
            if w.get(lx, ly, lz)[0] == 0:
                w.leaves(lx, ly, lz, wood)
        self.reserved.add((x, z))

    def near_path(self, x, z, r):
        # V8: any road or kerb cell within Chebyshev distance r
        for dx in range(-r, r + 1):
            for dz in range(-r, r + 1):
                p = (x + dx, z + dz)
                if p in self.road or p in self.kerb:
                    return True
        return False

    def tree_site_ok(self, x, z, r):
        g = self.ground(x, z)
        if g < 65 or self.bid(x, g, z) != 2:
            return False
        if self.near_reserved(x, z, 2):
            return False
        if any(x1 - r <= x <= x2 + r and z1 - r <= z <= z2 + r
               for (x1, z1, x2, z2) in self.boxes()):
            return False
        if not self.free_col(x, z, g + 1, 9):
            return False
        return all(self.bid(x + dx, y, z + dz) in self.PLAIN
                   for dx in range(-r, r + 1) for dz in range(-r, r + 1)
                   for y in range(g + 3, g + 11))

    def decoration(self):
        """V2 landscaping: the spec trees first (moved up to 10 cells to a
        clear spot or dropped), then a deterministic scatter of mixed
        woodland (spruce on the hill side, birch along the coast, oak and
        big oak inland), leaf bushes, flower patches, tall grass and ferns,
        dead bushes on the beach."""
        w = self.w
        placed = []

        def plant(px, pz, kind):
            self.tree_v2(px, pz, kind)
            placed.append((px, pz, kind))
            rr = 3 if kind == "bigoak" else 2
            for dx in range(-rr, rr + 1):
                for dz in range(-rr, rr + 1):
                    self.reserved.add((px + dx, pz + dz))

        for (x, z), kind in self.TREES:
            spot = None
            for r in range(0, 11):
                ring = [(x + dx, z + dz) for dx in range(-r, r + 1)
                        for dz in range(-r, r + 1) if max(abs(dx), abs(dz)) == r]
                ring.sort(key=lambda p: (abs(p[0] - x) + abs(p[1] - z), p))
                for (px, pz) in ring:
                    if self.tree_site_ok(px, pz, 3):
                        spot = (px, pz)
                        break
                if spot:
                    break
            if spot is None:
                self.dropped_trees.append(((x, z), kind, "no clear site within 10"))
                continue
            if spot != (x, z):
                self.moved_trees.append(((x, z), spot, kind))
            plant(spot[0], spot[1], kind)
        for gx in range(166, 352, 5):
            for gz in range(186, 330, 5):
                h = H(gx, gz, 40)
                x, z = gx + h % 3 - 1, gz + (h >> 4) % 3 - 1
                # V8: thinner woodland (30 % of the grid, was 46 %), and
                # no scatter tree within 4 of a path (clear sightlines)
                if (h >> 8) % 100 >= 30:
                    continue
                if self.near_path(x, z, 4):
                    continue
                rA = math.hypot((x - 256) / 80.0, (z - 256) / 70.0)
                roll = (h >> 12) % 100
                if x < 262 and z < 236:
                    kind = "spruce" if roll < 60 else ("oak" if roll < 85 else "birch")
                elif rA > 0.74:
                    kind = "birch" if roll < 50 else ("oak" if roll < 85 else "spruce")
                else:
                    kind = ("oak" if roll < 50 else "bigoak" if roll < 72
                            else "birch" if roll < 92 else "spruce")
                r = 3 if kind == "bigoak" else 2
                if any(abs(x - a) + abs(z - b) < (7 if kind == "bigoak" or k == "bigoak" else 5)
                       for (a, b, k) in placed):
                    continue
                if not self.tree_site_ok(x, z, r + 1):
                    continue
                plant(x, z, kind)
        self.n_trees = len(placed)
        # Bushes, flowers, grass.
        boxes = self.boxes()
        for x in range(160, 360):
            for z in range(180, 336):
                g = self.ground(x, z)
                top = self.bid(x, g, z)
                if top == 12 and g == 64 and self.bid(x, g + 1, z) == 0 \
                        and (x, z) not in self.reserved \
                        and H(x, z, 51) % 47 == 0 and not self.in_boxes(x, z, 1, True):
                    w.set(x, g + 1, z, 32)
                    continue
                if top != 2 or self.bid(x, g + 1, z) != 0:
                    continue
                # V8: bushes and flower patches keep 3 clear of paths and
                # 2 clear of buildings and their doors (uninterrupted grass)
                if self.near_reserved(x, z, 1) or any(
                        x1 - 2 <= x <= x2 + 2 and z1 - 2 <= z <= z2 + 2
                        for (x1, z1, x2, z2) in boxes):
                    continue
                if self.near_path(x, z, 3):
                    continue
                h = H(x, z, 7)
                if h % 61 == 0 and not self.near_reserved(x, z, 2) and \
                        self.free_col(x, z, g + 1, 3) and all(
                        self.bid(x + a, g + 1, z + b) in self.PLAIN
                        for a in (-1, 0, 1) for b in (-1, 0, 1)):
                    wood = ("oak", "birch", "spruce")[(h >> 8) % 3]
                    w.log(x, g + 1, z, wood)
                    for (a, b, c) in ((1, 1, 0), (-1, 1, 0), (0, 1, 1), (0, 1, -1),
                                      (0, 2, 0), (1, 2, 0), (0, 2, 1)):
                        if self.bid(x + a, g + b, z + c) == 0:
                            w.leaves(x + a, g + b, z + c, wood)
                    self.reserved.add((x, z))
                    continue
                seed = None
                for (a, b) in ((0, 0), (1, 0), (0, 1), (-1, 0), (0, -1), (2, 0),
                               (0, 2), (-2, 0), (0, -2), (1, 1), (-1, -1)):
                    if H(x + a, z + b, 13) % 157 == 0:
                        seed = (x + a, z + b)
                        break
                if seed is not None and h % 3 != 0:
                    w.flower(x, g + 1, z, "rose" if H(*seed, 14) % 2 else "dandelion")
                elif h % 97 == 0:
                    w.flower(x, g + 1, z, "rose" if (h >> 8) & 1 else "dandelion")
                elif h % 5 == 1:
                    w.tall_grass(x, g + 1, z, "grass")
                elif h % 23 == 2:
                    w.tall_grass(x, g + 1, z, "fern")

    # ================================================================ V7 ===
    # V7 pass (tmp/jobs/tutorial-1021/v7-plan.md): the gaps six reviews
    # agreed on. Each station is a furnished, lit build with its own zone
    # or footprint, so the landscaping passes keep out of it. Item ids are
    # the Item.java ones (constants at the top); every sign is verb-first
    # and names no controller button (the HUD shows the glyphs).

    def v7_kitchen(self):
        """Stop 8 farm kitchen, north of the field beside the trail: food
        heals at once and Beta 1.7.3 has no hunger bar (ItemFood heals on
        use; CaveWii refuses to eat at full health), so a cactus bed by its
        south wall (BlockCactus.onEntityCollidedWithBlock: 1 damage) lets
        the player lose a heart and eat it back."""
        w = self.w
        self.shed("s8_kitchen", 305, 222, 313, 228, 65, wall=(PLANKS, 0),
                  post="spruce", doors=[(305, 225)], trim="pink",
                  lower=(COBBLE, 0), chimney=(309, 223), mid_posts=4)
        w.door(305, 66, 225, "west")
        # the hearth: a lit furnace cooking pork, its flue in the chimney
        w.furnace(309, 66, 223, "south", lit=True,
                  items=[(0, PORK_RAW, 1), (1, COAL, 4), (2, PORK_COOKED, 1)],
                  burn_time=1600, cook_time=100)
        for y in (67, 68):
            w.set(309, y, 223, COBBLE)
        # work counters either side (a cake on one), the crafting table
        # for bread in the corner
        for x in (306, 307, 308, 310, 311):
            w.slab(x, 66, 223, "stone", double=True)
        w.set(307, 67, 223, 92)
        w.set(312, 66, 223, 58)
        # dining table (fence + wood slab top) with a stair chair each end
        for x in (309, 310):
            w.set(x, 66, 225, FENCE)
            w.slab(x, 67, 225, "wood")
        w.stairs(308, 66, 225, "west")
        w.stairs(311, 66, 225, "east")
        # pantry corner: a pumpkin and a barrel, the food chest
        w.pumpkin(306, 66, 227, "east")
        w.log(307, 66, 227, "spruce")
        self.chest(312, 66, 227, [
            (0, BREAD, 1), (1, BREAD, 1), (2, APPLE, 1), (3, APPLE, 1),
            (4, PORK_RAW, 1), (5, PORK_COOKED, 1), (6, PORK_COOKED, 1),
            (7, STEW, 1), (8, BOWL, 3), (9, 39, 3), (10, 40, 3),
            (11, WHEAT, 6), (12, COAL, 4)])
        self.sign(308, 67, 223, "EAT TO HEAL: / FOOD FILLS / HEARTS AT ONCE",
                  "south", wall=True)
        self.sign(310, 67, 223, "EAT WHEN HURT: / NO HUNGER BAR. / FULL "
                  "HEARTS = / NO EATING", "south", wall=True)
        self.sign(312, 67, 224, "COOK THE PORK: / COOKED FOOD / HEALS MORE",
                  "west", wall=True)
        self.sign(312, 67, 226, "MAKE STEW: / BOWL, RED AND / BROWN MUSHROOM",
                  "west", wall=True)
        self.sign(306, 67, 226, "BAKE BREAD: / 3 WHEAT IN / A ROW", "east",
                  wall=True)
        w.torch(306, 67, 224, "east")
        w.torch(312, 67, 225, "west")
        # cobble apron from the trail kerb to the door, lit both sides
        for x in (303, 304):
            for z in (224, 225, 226):
                w.set(x, 65, z, COBBLE)
                for y in (66, 67, 68):
                    w.set(x, y, z, 0)
                self.walk.add((x, 66, z))
                self.reserved.add((x, z))
        w.torch(304, 67, 224, "west")
        w.torch(304, 67, 226, "west")
        # cactus bed between the kitchen and the field fence: sand, a
        # 2-high cactus with air on all four sides (BlockCactus.
        # canBlockStay), cobble-slab kerbs, two dead bushes
        self.zones["cactus_bed"] = (306, 229, 310, 231)
        for x in range(306, 311):
            for z in range(229, 232):
                for y in range(66, 70):
                    w.set(x, y, z, 0)
                self.reserved.add((x, z))
                if x in (306, 310):
                    w.slab(x, 66, z, "cobblestone")
                    self.walk.discard((x, 66, z))
                else:
                    w.set(x, 65, z, SAND)
                    w.set(x, 64, z, SAND)
                    self.walk.add((x, 66, z))
        w.cactus(308, 66, 230, 2)
        for (x, z) in ((307, 229), (309, 231)):
            w.set(x, 66, z, 32)
        w.torch(309, 67, 229, "south")
        self.sign(311, 67, 229, "TOUCH CACTUS / TO LOSE A / HEART, THEN / "
                  "EAT TO HEAL", "south", wall=True)

    def v7_smithy(self):
        """Stop 7 smithy past the iron door: a forge (lit furnace under the
        chimney), an iron-block anvil, two double-slab plinths each holding
        a full armour set in a chest (leather 24, iron 24 = 5 + 8 + 7 + 4,
        Beta RecipesArmor), the recipe board, and a sample of every
        pickaxe tier (wood, stone, iron, diamond, gold)."""
        w = self.w
        self.shed("s7_smithy", 294, 202, 300, 208, 65, wall=(COBBLE, 0),
                  post="spruce", doors=[(297, 208)], trim="orange",
                  mossy=True, chimney=(297, 203), ridge="z")
        w.door(297, 66, 208, "south")
        w.furnace(297, 66, 203, "south", lit=True,
                  items=[(0, 15, 2), (1, COAL, 4), (2, IRON_INGOT, 2)],
                  burn_time=1600, cook_time=60)
        for y in (67, 68):
            w.set(297, y, 203, COBBLE)
        w.slab(296, 66, 203, "stone", double=True)
        w.slab(298, 66, 203, "stone", double=True)
        w.set(295, 66, 203, 58)
        self.chest(299, 66, 203, [(0, IRON_INGOT, 24), (1, LEATHER, 24),
                                  (2, STICK, 8), (3, COAL, 8)])
        w.set(299, 66, 205, 42)                    # the anvil: iron block
        for (z, kit, text) in (
                (205, LEATHER_ARMOUR, "WEAR ARMOUR TO / TAKE LESS HARM: "
                 "/ LEATHER IS / LIGHT, CHEAP"),
                (207, IRON_ARMOUR, "WEAR IRON FOR / BETTER CARE: / 24 "
                 "INGOTS MAKE / A FULL SET")):
            w.slab(295, 66, z, "stone", double=True)
            self.chest(295, 67, z, [(i, it, 1) for i, it in enumerate(kit)])
            self.sign(295, 68, z, text, "east", wall=True)
        w.torch(295, 67, 206, "east")
        self.chest(299, 66, 207, [(0, WOOD_PICK, 1), (1, STONE_PICK, 1),
                                  (2, IRON_PICK, 1), (3, DIAMOND_PICK, 1),
                                  (4, GOLD_PICK, 1)])
        self.sign(299, 67, 204, "CRAFT ARMOUR: / 5 = HELMET / 8 = CHESTPLATE "
                  "/ 7 LEGS, 4 BOOTS", "west", wall=True)
        self.sign(299, 67, 206, "UPGRADE TOOLS: / WOOD, STONE, / IRON, "
                  "DIAMOND", "west", wall=True)
        self.sign(299, 68, 205, "SKIP GOLD: / FAST, BUT IT / WEARS OUT SOON",
                  "west", wall=True)
        self.sign(299, 68, 207, "USE AN IRON / PICK FOR / DIAMOND, GOLD / "
                  "AND REDSTONE", "west", wall=True)
        for x in (296, 298):
            w.torch(x, 67, 207, "north")
            w.torch(x, 67, 209, "south")
        for z in (209, 210, 211):
            w.set(297, 65, z, COBBLE)
            for y in (66, 67, 68):
                w.set(297, y, z, 0)
            self.walk.add((297, 66, z))
            self.reserved.add((297, z))

    def v7_fluids(self):
        """Buckets, water and lava on the quarry's east lip (beside L5,
        under the bench wall): a still water pool with a slab rim and the
        bucket chest; an obsidian / cobble plinth for what water does to
        lava; a 2-block lava pool sunk flush in the paving under a glass
        lid that covers it and its rim, so it cannot flow (stone on every
        side and below) and cannot light anything (BlockStationary lava
        fire walk: the first step up is the glass, a solid material, so
        it returns; and nothing flammable within 3)."""
        w = self.w
        x1, z1, x2, z2 = 247, 209, 253, 221
        self.zones["fluids"] = (x1, z1, x2, z2)
        for x in range(x1, x2 + 1):
            for z in range(z1, z2 + 1):
                for y in range(66, 75):
                    w.set(x, y, z, 0)
                w.set(x, 64, z, 1)
                edge = x in (x1, x2) or z in (z1, z2)
                w.set(x, 65, z, COBBLE if edge else 1)
                self.walk.add((x, 66, z))
                self.reserved.add((x, z))
        # water pool x 248..250, z 211..212, rimmed by cobble slabs
        for x in range(247, 252):
            for z in range(210, 214):
                if 248 <= x <= 250 and 211 <= z <= 212:
                    w.water_source(x, 65, z)
                else:
                    w.slab(x, 66, z, "cobblestone")
                self.walk.discard((x, 66, z))
        self.chest(252, 66, 211, [(0, BUCKET, 1), (1, BUCKET, 1),
                                  (2, BUCKET, 1)])
        self.sign(252, 66, 212, "FILL A BUCKET / AT THE POOL, / POUR IT, "
                  "THEN / SCOOP IT BACK", "east")
        # what water does to lava (BlockFlowing.checkForHarden)
        for (x, top) in ((250, OBSIDIAN), (251, COBBLE)):
            w.slab(x, 66, 215, "stone", double=True)
            w.set(x, 67, 215, top)
        self.sign(252, 66, 215, "POUR WATER ON / LAVA: STILL = / OBSIDIAN, "
                  "FLOW / = COBBLESTONE", "east")
        # the lava pool under its glass lid
        for x in (248, 249):
            w.lava_source(x, 65, 217)
        for x in range(247, 251):
            for z in range(216, 219):
                w.set(x, 66, z, GLASS)
        self.sign(251, 66, 217, "LOOK, DON'T / TOUCH: LAVA / BURNS YOU AND "
                  "/ YOUR ITEMS", "east")
        self.sign(251, 66, 220, "DIG STAIRS, / NOT STRAIGHT / DOWN: LAVA / "
                  "HIDES BELOW", "east")
        for p in ((252, 66, 211), (252, 66, 212), (250, 66, 215),
                  (251, 66, 215), (252, 66, 215), (251, 66, 217),
                  (251, 66, 220)):
            self.walk.discard(p)
        for z in (214, 220):
            w.torch(247, 67, z, "east")            # on the bench wall
        for z in (z1, z2):
            self.lamp(x2, z, 65)
            self.walk.discard((x2, 66, z))

    def v7_nether_gate(self):
        """Optional Nether gate, east of L5 below gate 6 and set back from
        the trail: a small walled court behind a wooden door with a warning
        sign, an UNLIT 4 x 5 obsidian frame (never lit by the build: no
        portal or fire block anywhere), the flint and steel in a chest."""
        w = self.w
        x1, z1, x2, z2 = 261, 216, 267, 222
        self.footprints["nether_gate"] = (x1, z1, x2, z2)
        self.house_meta["nether_gate"] = {"fy": 65, "wall_h": 3,
                                          "upper": None, "ridge": "x",
                                          "windows": False}
        self.soft["nether_gate_yard"] = (x1 - 3, z1 - 3, x2 + 3, z2 + 3)
        self.foundation(x1, z1, x2, z2, 65, COBBLE)
        for x in range(x1, x2 + 1):
            for z in range(z1, z2 + 1):
                if x in (x1, x2) or z in (z1, z2):
                    for y in (66, 67, 68):
                        w.set(x, y, z, 48 if H(x, z, y) % 4 == 0 else COBBLE)
                    w.slab(x, 69, z, "cobblestone")
                else:
                    self.walk.add((x, 66, z))
        for y in (66, 67):
            w.set(x1, y, 219, 0)
        w.door(x1, 66, 219, "west")
        for z in range(218, 222):                  # the frame, plane x=265
            for y in range(66, 71):
                if z in (218, 221) or y in (66, 70):
                    w.set(265, y, z, OBSIDIAN)
        self.chest(262, 66, 221, [(0, FLINT_STEEL, 1)])
        self.sign(262, 67, 221, "LIGHT THE / FRAME WITH / FLINT AND / STEEL",
                  "east", wall=True)
        self.sign(263, 67, 217, "PACK FOOD, / ARMOUR AND / TORCHES FIRST",
                  "south", wall=True)
        self.sign(265, 67, 217, "BUILD A FRAME: / 10 OBSIDIAN, / 4 WIDE, "
                  "5 TALL", "south", wall=True)
        self.sign(260, 67, 218, "ENTER ONLY IF / READY: THE / NETHER IS / "
                  "DANGEROUS", "west", wall=True)
        w.torch(262, 67, 218, "east")
        w.torch(266, 67, 219, "west")
        w.torch(264, 67, 221, "north")
        w.torch(260, 67, 220, "west")
        for x in (259, 260):
            for z in (218, 219, 220):
                w.set(x, 65, z, COBBLE)
                for y in (66, 67, 68):
                    if (x, y, z) not in ((260, 67, 218), (260, 67, 220)):
                        w.set(x, y, z, 0)
                self.walk.add((x, 66, z))
                self.reserved.add((x, z))

    def v7_range(self):
        """Stop 11 combat range on the lab's south terrace (final, lifted
        coordinates: grass at y 68): an opening in the terrace wall from
        L10, the bow chest, a mob board (four signs on a plank board
        between log posts), a cobble shooting line and a 5 x 5 wool
        target between log posts 11 blocks down the lane. No spawner."""
        w = self.w
        g = 68
        self.zones["range"] = (247, 285, 263, 293)
        for z in (285, 286):                       # the way in from L10
            for y in range(g + 1, g + 4):
                w.set(247, y, z, 0)
            w.set(247, g, z, COBBLE)
            self.walk.add((247, g + 1, z))
        for x in range(248, 264):
            for z in range(285, 294):
                if self.bid(x, g, z) == 2 and self.free_col(x, z, g + 1, 2):
                    self.walk.add((x, g + 1, z))
                self.reserved.add((x, z))
        self.post_sign(249, g + 1, 285, "TAKE THE BOW / FROM THE CHEST / "
                       "AND SHOOT THE / TARGET", "west")
        self.chest(250, g + 1, 285, [(0, BOW, 1), (1, ARROW, 32),
                                     (2, STONE_SWORD, 1)])
        # the mob board on the west side, facing down the lane
        for z in (287, 292):
            for y in range(g + 1, g + 4):
                w.log(248, y, z, "spruce")
            w.torch(248, g + 4, z, "floor")
        for z in range(288, 292):
            for y in (g + 1, g + 2):
                w.set(248, y, z, PLANKS)
            w.slab(248, g + 3, z, "wood")
        for z, text in ((288, "FIGHT ZOMBIES: / THEY CHASE / YOU AND HIT"),
                        (289, "DODGE / SKELETONS: / THEY SHOOT / ARROWS"),
                        (290, "WATCH SPIDERS: / THEY CLIMB / WALLS"),
                        (291, "RUN FROM / CREEPERS: THEY / HISS, THEN / "
                              "EXPLODE")):
            self.sign(249, g + 2, z, text, "east", wall=True)
        # shooting line
        for z in range(287, 293):
            w.set(251, g, z, COBBLE)
        # the target: log posts, a 5 x 5 red / white / yellow bullseye
        # facing west, a wood-slab cap, torches on the posts
        rows = ("RRRRR", "RWWWR", "RWYWR", "RWWWR", "RRRRR")
        col = {"R": "red", "W": "white", "Y": "yellow"}
        for z in (286, 292):
            for y in range(g + 1, g + 6):
                w.log(262, y, z, "spruce")
            w.torch(262, g + 6, z, "floor")
        for i, row in enumerate(rows):
            for j, ch in enumerate(row):
                w.wool(262, g + 5 - i, 287 + j, col[ch])
        for z in range(287, 292):
            w.slab(262, g + 6, z, "wood")
        w.torch(263, g + 2, 289, "east")           # lights the strip behind
        # lane lights: fence posts with torches outside the lane
        for x in (250, 254, 258, 261):
            for z in (286, 293):
                w.set(x, g + 1, z, FENCE)
                w.torch(x, g + 2, z, "floor")

    def check_leaves(self):
        """Every leaf within 4 leaf-steps of a log (BlockLeaves.java:56)."""
        w = self.w
        bad = []
        for (cx, cz), c in self.w.chunks.items():
            if 18 not in c.blocks:
                continue
            for i, b in enumerate(c.blocks):
                if b != 18:
                    continue
                x, z, y = (cx << 4) | (i >> 11), (cz << 4) | ((i >> 7) & 15), i & 127
                frontier, seen, found = [(x, y, z)], {(x, y, z)}, False
                for _ in range(4):
                    nxt = []
                    for (a, b2, cc) in frontier:
                        for d in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0),
                                  (0, 0, 1), (0, 0, -1)):
                            p = (a + d[0], b2 + d[1], cc + d[2])
                            if p in seen:
                                continue
                            seen.add(p)
                            t = w.get(*p)[0]
                            if t == 17:
                                found = True
                            elif t == 18:
                                nxt.append(p)
                    if found:
                        break
                    frontier = nxt
                if not found:
                    bad.append((x, y, z))
        return bad

    # ------------------------------------------------------------- light ---
    def dark_walk_cells(self):
        by_col = {}
        for (x, y, z) in self.walk:
            by_col.setdefault((x, z), set()).add(y)
        out = []
        for (x, z) in sorted(by_col):
            ys = by_col[(x, z)]
            for (sx, sy, sz) in self.w.dark_spawn_spots(x, z, x, z):
                if sy in ys:
                    out.append((sx, sy, sz))
        return out

    def dim_growth_cells(self):
        return [p for p in sorted(self.growth) if self.w.light_at(*p)[1] < 9]

    def place_fix_torch(self, x, y, z):
        """Light a failing feet cell (x,y,z): wall torch at head height on an
        adjacent opaque wall within 3, else a fence+torch post on a nearby
        non-walkable cell."""
        w = self.w
        cand = []
        for r in range(0, 4):
            for dx in range(-r, r + 1):
                for dz in range(-r, r + 1):
                    if max(abs(dx), abs(dz)) != r:
                        continue
                    cx, cz = x + dx, z + dz
                    hy = y + 1
                    if w.get(cx, hy, cz)[0] != 0:
                        continue
                    if (solid(w.get(cx - 1, hy, cz)[0]) and solid(w.get(cx + 1, hy, cz)[0])) or                             (solid(w.get(cx, hy, cz - 1)[0]) and solid(w.get(cx, hy, cz + 1)[0])):
                        continue   # doorway / 1-wide gap: keep it clear
                    for f, (ox, oz) in (("east", (-1, 0)), ("west", (1, 0)),
                                        ("south", (0, -1)), ("north", (0, 1))):
                        if solid(w.get(cx + ox, hy, cz + oz)[0]) and \
                                w.get(cx + ox, hy, cz + oz)[0] not in (54, 58, 61, 62):
                            cand.append((r, 0, cx, hy, cz, f))
            if cand:
                break
        if cand:
            cand.sort()
            _, _, cx, hy, cz, f = cand[0]
            w.torch(cx, hy, cz, f)
            self.fix_torches.append(("wall", cx, hy, cz))
            return True
        for r in range(1, 5):
            for dx in range(-r, r + 1):
                for dz in range(-r, r + 1):
                    cx, cz = x + dx, z + dz
                    if (cx, cz) in self.road:
                        continue
                    if (cx, y, cz) in self.walk and not any(
                            x1 <= cx <= x2 and z1 <= cz <= z2
                            for n, (x1, z1, x2, z2) in self.zones.items()
                            if n in ("farm", "buildpad", "quarry", "bays")):
                        continue
                    if w.get(cx, y, cz)[0] not in (0, 31, 37, 38, 44) or \
                            w.get(cx, y + 1, cz)[0] != 0 or \
                            w.get(cx, y + 2, cz)[0] != 0 or \
                            not solid(w.get(cx, y - 1, cz)[0]):
                        continue
                    # V3: a cobble lantern, never a fence + torch post
                    self.lamp(cx, cz, y - 1)
                    self.walk.discard((cx, y, cz))
                    self.fix_torches.append(("lantern", cx, y, cz))
                    return True
        return False

    def light_pass(self, cap=200):
        self.w.compute_light()
        self.light_sealed()
        for _ in range(6):
            dark = self.dark_walk_cells()
            dim = self.dim_growth_cells()
            if not dark and not dim:
                return
            fixed_any = False
            covered = set()
            for (x, y, z) in dark + [(x, y - 1, z) for (x, y, z) in dim]:
                if any(abs(x - a) + abs(z - c) <= 3 and abs(y - b) <= 1
                       for (a, b, c) in covered):
                    continue
                if len(self.fix_torches) >= cap:
                    raise AssertionError(
                        "light pass needs more than %d extra torches; layout "
                        "is wrong near %s" % (cap, (x, y, z)))
                if self.place_fix_torch(x, y, z):
                    covered.add((x, y, z))
                    fixed_any = True
            if not fixed_any:
                break
            self.w.compute_light()
        dark = self.dark_walk_cells()
        dim = self.dim_growth_cells()
        if dark or dim:
            raise AssertionError("unlit walkable cells: %s; dim growth: %s"
                                 % (dark[:20], dim[:20]))

    # ------------------------------------------------------------ build ----
    def build(self):
        self.terrain()
        self.check_frame()
        self.quarry_carve()
        self.roads()
        self.stop1_beach()
        self.stop2_woods()
        self.stop3_crafting()
        self.stop4_tools()
        self.stop5_mine()
        self.stop6_torches()
        self.stop7_smelting()
        self.stop8_farm()
        self.stop9_cottage()
        self.stop10_bedhouse()
        self.hub()
        self.redstone_hall()
        self.cart_yard()
        self.monster_lab()
        self.bridge()
        self.islet()
        self.harbour()
        self.lighthouse()
        self.town()
        self.windmill()
        self.v7_kitchen()
        self.v7_smithy()
        self.v7_nether_gate()
        # V3: verticality (rigid moves), then the landmarks that sit on it
        self.lower_mine()
        self.lift_hill()
        self.station_eave()
        self.castle_bands()
        self.ramps()
        self.hub_exits()
        self.retain()
        self.quarry_benches()
        self.v7_fluids()
        self.v7_range()
        self.mine_reladder()
        self.details()
        self.cavex_sign()
        self.landing_map()
        self.ponds()
        # V3: wayfinding spine
        self.road_style()
        self.gateways()
        self.trail_end()
        self.stripes_and_arrows()
        self.forecourts()
        self.relief()
        self.despeckle()
        self.edging()
        self.walk_roads()
        self.decoration()
        self.windows()
        # Walkable cells are only the ones a mob could stand in: drop cells
        # that the build filled.
        self.walk = {(x, y, z) for (x, y, z) in self.walk
                     if self.w.get(x, y, z)[0] == 0}
        bad = self.check_leaves()
        assert not bad, "leaves without a log within 4: %s" % bad[:10]
        self.light_pass()
        # 2.14: the dark cell must really be dark (zombies need light <= 7,
        # EntityMob.java:84) and the lit cell must never be.
        cell_y = 66 + self.hub_dy
        dark_cell = [p for p in self.w.dark_spawn_spots(249, 277, 253, 281)
                     if p[1] == cell_y]
        lit_cell = [p for p in self.w.dark_spawn_spots(259, 277, 263, 281)
                    if p[1] == cell_y]
        assert len(dark_cell) >= 10, "dark cell too bright: %d spots" % len(dark_cell)
        assert not lit_cell, "lit cell has dark spots %s" % lit_cell
        self.dark_cell_spots = len(dark_cell)
        self.pen_animals()
        return self.w

    def pen_animals(self):
        """V6c: 2 sheep (one pink), 2 cows, 2 pigs, 2 chickens in the stop 8
        pen, all in chunk (19, 15) (x 304..319, z 240..255) so only that
        chunk is written as a pure-Beta chunk (worldgen_lib.chunk_nbt).
        Placed last, after the light pass, and each one checked: grass
        under its feet, air in its feet and head cells, inside the fence."""
        w = self.w
        for (x, z, kind, opts) in PEN_ANIMALS:
            assert w.get(x, 65, z)[0] == 2, "pen animal not on grass"
            assert w.get(x, 66, z)[0] == 0 and w.get(x, 67, z)[0] == 0
            assert PEN_BOX[0] < x < PEN_BOX[2] and PEN_BOX[1] < z < PEN_BOX[3]
            assert (x >> 4, z >> 4) == (19, 15)
            w.mob(x + 0.5, 66.0, z + 0.5, kind, **opts)


# ------------------------------------------------------------- checks ------
_WHERE = re.compile(r"(\d+)@\((-?\d+),(-?\d+),(-?\d+)\)")
_TE_AT = re.compile(r"tile entity without its block at \((-?\d+),(-?\d+),(-?\d+)\)")


def check_problems(w):
    """World.problems() minus two verified library false positives, plus the
    builder's own rules. Library gaps (to fix in worldgen_lib.problems):
      * a floor torch (50 meta 5) on a FENCE is legal (BlockTorch.java:28)
        but problems() requires an opaque block below;
      * MobSpawner (52) tile entities are not in problems()' TE table.
    Each filtered message is re-verified here."""
    out = []
    for p in w.problems():
        m = _WHERE.search(p)
        if p.startswith("no mount block") and m and m.group(1) == "50":
            x, y, z = (int(m.group(i)) for i in (2, 3, 4))
            if w.get(x, y, z)[1] == 5 and w.get(x, y - 1, z)[0] == FENCE:
                continue
        t = _TE_AT.search(p)
        if t:
            x, y, z = (int(t.group(i)) for i in (1, 2, 3))
            te = w.tile_entity(x, y, z)
            if (w.get(x, y, z)[0] == 52 and wl.child(te, "id").value == "MobSpawner"
                    and wl.child(te, "EntityId") is not None
                    and wl.child(te, "Delay").type == wl.TAG_SHORT):
                continue
        out.append(p)
    spawners = 0
    for (cx, cz), c in sorted(w.chunks.items()):
        if max(c.blocks) > 96:
            out.append("block id > 96 in chunk %d,%d" % (cx, cz))
        for i, b in enumerate(c.blocks):
            if b == 52:
                spawners += 1
                x, z, y = (cx << 4) | (i >> 11), (cz << 4) | ((i >> 7) & 15), i & 127
                te = c.tile_entities.get((x, y, z))
                if te is None or wl.child(te, "id").value != "MobSpawner":
                    out.append("spawner without MobSpawner TE at %d,%d,%d" % (x, y, z))
            if b == 54:
                x, z, y = (cx << 4) | (i >> 11), (cz << 4) | ((i >> 7) & 15), i & 127
                items = wl.child(c.tile_entities[(x, y, z)], "Items")
                if not items.value:
                    out.append("empty chest at %d,%d,%d" % (x, y, z))
            if b in (63, 68):
                x, z, y = (cx << 4) | (i >> 11), (cz << 4) | ((i >> 7) & 15), i & 127
                te = c.tile_entities[(x, y, z)]
                for k in range(1, 5):
                    ln = wl.child(te, "Text%d" % k).value
                    if len(ln) > 15 or ln != ln.upper():
                        out.append("bad sign line %r at %d,%d,%d" % (ln, x, y, z))
    if spawners > 2:
        out.append("%d spawners (max 2)" % spawners)
    # V7: Beta stack sizes in every chest; lava contained and fire-safe;
    # no portal or fire block (the Nether frame stays unlit).
    lava = []
    for (cx, cz), c in sorted(w.chunks.items()):
        for (x, y, z), te in sorted(c.tile_entities.items()):
            if wl.child(te, "id").value != "Chest":
                continue
            for it in wl.child(te, "Items").value:
                iid, n = wl.child(it, "id").value, wl.child(it, "Count").value
                if n > max_stack(iid):
                    out.append("chest %d,%d,%d: item %d x%d over stack %d"
                               % (x, y, z, iid, n, max_stack(iid)))
        if 51 in c.blocks or 90 in c.blocks:
            out.append("fire or portal block in chunk %d,%d" % (cx, cz))
        if 10 in c.blocks or 11 in c.blocks:
            for i, b in enumerate(c.blocks):
                if b in (10, 11):
                    lava.append(((cx << 4) | (i >> 11), i & 127,
                                 (cz << 4) | ((i >> 7) & 15), b))
    for (x, y, z, b) in lava:
        if b != 11:
            out.append("flowing lava at %d,%d,%d" % (x, y, z))
        for (a, d, e) in ((1, 0, 0), (-1, 0, 0), (0, 0, 1), (0, 0, -1),
                          (0, -1, 0)):
            nb = w.get(x + a, y + d, z + e)[0]
            if nb not in (10, 11) and not solid(nb):
                out.append("lava at %d,%d,%d can flow into %d" % (x, y, z, nb))
        # BlockStationary.updateTick (lava): each fire step goes up one and
        # sideways by -1..1; a solid block there ends the walk. A full lid
        # over the 3x3 means it can never reach air.
        for a in (-1, 0, 1):
            for e in (-1, 0, 1):
                if w.get(x + a, y + 1, z + e)[0] not in (GLASS, 1, 4):
                    out.append("lava at %d,%d,%d has no lid at %d,%d"
                               % (x, y, z, x + a, z + e))
        for a in range(-3, 4):
            for e in range(-3, 4):
                for d in range(-1, 4):
                    nb = w.get(x + a, y + d, z + e)[0]
                    if nb in FLAMMABLE:
                        out.append("flammable %d near lava at %d,%d,%d"
                                   % (nb, x + a, y + d, z + e))
    # V6c pen: the 8 animals stand on grass inside the fence ring, the ring
    # is closed except the door, and no monster can spawn inside it.
    px1, pz1, px2, pz2 = PEN_BOX
    mobs = sorted(wl.child(e, "id").value for e in w.entities())
    if mobs != ["Chicken"] * 2 + ["Cow"] * 2 + ["Pig"] * 2 + ["Sheep"] * 2:
        out.append("pen animals: %s" % mobs)
    for e in w.entities():
        ex, ey, ez = (v.value for v in wl.child(e, "Pos").value)
        bx, bz = int(ex // 1), int(ez // 1)
        if not (px1 < bx < px2 and pz1 < bz < pz2) or ey != 66.0 \
                or w.get(bx, 65, bz)[0] != 2:
            out.append("pen animal off the pen grass at %.1f,%.1f" % (ex, ez))
    for x in range(px1, px2 + 1):
        for z in range(pz1, pz2 + 1):
            if (x in (px1, px2) or z in (pz1, pz2)) and \
                    w.get(x, 66, z)[0] not in (FENCE, 64):
                out.append("pen fence gap at %d,%d" % (x, z))
    if w.get(px1, 66, 240)[0] != 64 or w.get(px1, 67, 240)[0] != 64:
        out.append("pen door missing")
    if w.chunk(px1 >> 4, pz1 >> 4, create=False).sky is not None:
        dark = w.dark_spawn_spots(px1 + 1, pz1 + 1, px2 - 1, pz2 - 1)
        if dark:
            out.append("dark spawn spots in the pen: %s" % dark)
    return out


def build():
    t = Tutorial()
    t.build()
    return t


def write_previews(t, out_dir):
    import preview
    w = t.w
    os.makedirs(out_dir, exist_ok=True)

    def crop(x0, z0, x1, z1, scale, layer=None):
        wd, ht = (x1 - x0 + 1) * scale, (z1 - z0 + 1) * scale
        rows = [bytearray(wd * 3) for _ in range(ht)]
        for z in range(z0, z1 + 1):
            for x in range(x0, x1 + 1):
                c = w.chunks.get((x >> 4, z >> 4))
                rgb = preview.VOID
                if c is not None:
                    base = ((x & 15) << 11) | ((z & 15) << 7)
                    if layer is None:
                        for y in range(127, -1, -1):
                            b = c.blocks[base + y]
                            if b:
                                f = 0.55 + 0.45 * ((y - 40) / 40.0)
                                rgb = preview._shade(preview.colour(b, c.meta[base + y]),
                                                     max(0.4, min(1.25, f)))
                                break
                    else:
                        b = c.blocks[base + layer]
                        rgb = preview.colour(b, c.meta[base + layer]) if b else \
                            preview.AIR_SLICE
                px = bytes(rgb) * scale
                for dy in range(scale):
                    r = rows[(z - z0) * scale + dy]
                    r[(x - x0) * scale * 3:(x - x0 + 1) * scale * 3] = px
        if scale >= 6:
            for z in range(z0, z1 + 1):
                for x in range(x0, x1 + 1):
                    if x % 16 == 0:
                        for dy in range(scale):
                            r = rows[(z - z0) * scale + dy]
                            r[(x - x0) * scale * 3:(x - x0) * scale * 3 + 3] = b"\x00\x00\x00"
                    if z % 16 == 0:
                        r = rows[(z - z0) * scale]
                        r[(x - x0) * scale * 3:(x - x0 + 1) * scale * 3] = \
                            b"\x00\x00\x00" * scale
        return wd, ht, rows

    def side_by_side(parts):
        gap = 8
        wd = sum(p[0] for p in parts) + gap * (len(parts) - 1)
        ht = max(p[1] for p in parts)
        rows = [bytearray(b"\x10\x10\x18" * wd) for _ in range(ht)]
        ox = 0
        for (pw, ph, pr) in parts:
            for i in range(ph):
                rows[i][ox * 3:(ox + pw) * 3] = pr[i]
            ox += pw + gap
        return wd, ht, rows

    paths = []
    p = os.path.join(out_dir, "tutorial_top.png")
    preview.write_png(p, *crop(160, 180, 368, 330, 6))
    paths.append(p)
    zones = {
        "beach": ((166, 284, 216, 328), (64,)),
        "woods": ((186, 256, 214, 286), (66,)),
        "crafting": ((180, 236, 210, 254), (66,)),
        "tools": ((198, 214, 222, 232), (66,)),
        "mine": ((210, 198, 242, 234), (63, 38, 10)),
        "torch_tunnel": ((250, 194, 286, 212), (66,)),
        "smelting": ((280, 202, 300, 220), (66,)),
        "farm": ((298, 228, 326, 256), (66,)),
        "cottage": ((304, 262, 328, 282), (66,)),
        "bed_house": ((284, 288, 304, 306), (66,)),
        "hub": ((238, 238, 274, 274), (72,)),
        "redstone_hall": ((242, 222, 270, 246), (72, 71)),
        "cart_yard": ((202, 242, 246, 270), (69,)),
        "monster_lab": ((242, 266, 268, 292), (72,)),
        "bridge_islet": ((306, 200, 360, 226), (66, 65, 64)),
        "cavex_sign": ((156, 224, 170, 288), (100,)),
        "town": ((266, 240, 300, 272), (69,)),
        "windmill": ((262, 276, 292, 300), (66,)),
        "landing": ((192, 286, 232, 310), (65,)),
        "trail_end": ((170, 292, 200, 312), (71,)),
    }
    for name, ((x0, z0, x1, z1), layers) in zones.items():
        sc = 12 if (x1 - x0) <= 40 else 9
        parts = [crop(x0, z0, x1, z1, sc)] + [crop(x0, z0, x1, z1, sc, y)
                                              for y in layers]
        p = os.path.join(out_dir, "zone_%s.png" % name)
        preview.write_png(p, *side_by_side(parts))
        paths.append(p)
    return paths


def summary(t):
    w = t.w
    n_blocks = sum(len(c.blocks) - c.blocks.count(0) for c in w.chunks.values())
    return {
        "chunks": len(w.chunks), "non_air_blocks": n_blocks,
        "signs": len(t.signs), "chests": len(t.chests),
        "spawners": len(t.spawners), "lamp_posts": t.lamp_posts,
        "fix_torches": len(t.fix_torches), "dropped_trees": len(t.dropped_trees),
        "moved_trees": len(t.moved_trees), "dark_cell_spots": t.dark_cell_spots,
        "walkable_cells": len(t.walk),
        "tile_entities": sum(len(c.tile_entities) for c in w.chunks.values()),
        "mobs": sum(len(c.entities) for c in w.chunks.values()),
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--previews", default=None,
                    help="directory for tutorial_top.png + zone_*.png")
    ap.add_argument("--summary", default=None,
                    help="also write the summary as 'key value' lines to this "
                         "file (read by tests/test_tutorial_world_load.c --real)")
    a = ap.parse_args(argv)
    t = build()
    bad = check_problems(t.w)
    if bad:
        raise SystemExit("problems:\n  " + "\n  ".join(bad[:50]))
    files = t.w.save(a.out, light=False)
    for k, v in summary(t).items():
        print("%-16s %s" % (k, v))
    if a.summary:
        with open(a.summary, "w") as f:
            for k, v in summary(t).items():
                f.write("%s %s\n" % (k, v))
    for (pos, kind, why) in t.dropped_trees:
        print("dropped tree", pos, kind, why)
    for ft in t.fix_torches:
        print("light-pass torch", ft)
    for rel in ["level.dat"] + [os.path.join("region", f) for f in files]:
        with open(os.path.join(a.out, rel), "rb") as f:
            print("sha256 %s %s" % (hashlib.sha256(f.read()).hexdigest()[:16], rel))
    print("spec deviations: %d (see DEVIATIONS)" % len(DEVIATIONS))
    if a.previews:
        for p in write_previews(t, a.previews):
            print("preview", p)


if __name__ == "__main__":
    main()
