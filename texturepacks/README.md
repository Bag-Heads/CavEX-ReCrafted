# texturepacks/

Drop user-supplied texture packs here, one folder per pack
(e.g. `texturepacks/mypack/`), then set `paths.texturepack` in
`config_wii.json` to `texturepacks/mypack`.

A pack only needs the files it overrides — anything it omits falls back to the
bundled default pack in `assets/`.

CaveWii ships **no** official textures. See `docs/texture_packs.md` for the
pack layout, fallback behaviour, HD-pack limits, and `tools/make_beta_pack.ps1`
for converting authentic Java Beta 1.7.3 assets into a pack.

This folder is intentionally empty in the repo except for this README — packs
are user-supplied and must not be committed.

**Optional Isabella II pack.** The release zip also puts one ready-made pack
here: `texturepacks/isabella-ii/` (Isabella II by Bonemouse, CC BY 3.0
Unported; its tracked source is `packs/isabella-ii/` in the repo, not this
folder). Pick it on the Texture Packs screen, where it is listed as
`isabella-ii`. It is a PARTIAL pack: it holds 11 Isabella images plus its
own original CC0 pointer and font, and every file it
lacks comes from the bundled default art in `assets/`. Its own `README.txt`
has the credit and licence. The bundled default art in `assets/` is Earth &
Ash (original CC0 art, since 2026-10-10). Adding or importing your own Beta 1.7.3 pack works as described above.
