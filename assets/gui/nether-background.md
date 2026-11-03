# Nether loading scene

Generated locally with this pack's textures by the native CaveWii engine. Seed 42; eye (-59.5,36.62,100.5); yaw 90; polar pitch 78. Lava sea, netherrack cliffs and glowstone. Fixed row-major population, no fluid ticks or lavafalls, no inserted blocks or painted pixels. This is not a live-world parity capture. See the adjacent provenance JSON for input/output hashes. Texture rights remain with their original owners.

Earth & Ash textures and this render: CC0 1.0.

Portal default on, selected-pack image only; absent/corrupt image falls back to tiled active-pack netherrack. Same beach black wash (alpha 72). Loaded and kept resident exactly like the beach menu image. Packs without this file bake the same recipe at the title, cache it on SD, and retain the decoded texture. Portal frames perform no IO, decode, allocation, bake, fence or release. Native PC preview, hardware-unconfirmed.
