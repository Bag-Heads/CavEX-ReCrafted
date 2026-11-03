# Nether loading scene

Generated locally with this pack's textures by the native CaveWii engine. Seed 42; eye (-59.5,36.62,100.5); yaw 90; polar pitch 78. Lava sea, netherrack cliffs and glowstone. Fixed row-major population, no fluid ticks or lavafalls, no inserted blocks or painted pixels. This is not a live-world parity capture. See the adjacent provenance JSON for input/output hashes. Texture rights remain with their original owners.

Isabella II by Bonemouse, CC BY 3.0; render by CaveWii contributors under CC BY 3.0. Retain LICENSE.txt and About Isabella.txt. Original: http://www.minecraftforum.net/topic/242175-Isabella/ . License: https://creativecommons.org/licenses/by/3.0/ .

Portal default on, selected-pack image only; absent/corrupt image falls back to tiled active-pack netherrack. Same beach black wash (alpha 72). Loaded and kept resident exactly like the beach menu image. Packs without this file bake the same recipe at the title, cache it on SD, and retain the decoded texture. Portal frames perform no IO, decode, allocation, bake, fence or release. Native PC preview, hardware-unconfirmed.
