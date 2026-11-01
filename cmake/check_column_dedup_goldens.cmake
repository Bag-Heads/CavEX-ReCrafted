# ci-1101/12 render FAST re-pin: P=354809385; accepted changes, old pins
# and first divergence in manifest.json ci_1101_12_render_repin.
# ci-1101/12 state FAST re-pin: P=354809385; accepted changes, old pins
# and first divergence in manifest.json ci_1101_12_state_repin.
# G9X-6 re-pin (maintainer ruling 3, 2026-10-14): Beta-published light republish
# (CRPC_CHUNK refresh of already-sent columns; server traces and seed-42 render
# byte-identical), evidence tmp/jobs/loop/G9X-6/RERECORD.md. G9X-6 hashes:
# ms2_12345_client.gz 664c35f577003a81697080217391d7e9c30db5749f76eb722f277c440467ea6c
# ms2_12345_render.gz 90ed95c15ec4389741ae38d33f2bc1fbb14a5b606db6380d790731a6911f3752
# ms2_42_client.gz 9ab89273df80881bd85c52bc574c29817ab3b0308f33c543a26974e0c3c12194
# Previous ms2_12345_client.gz 88bfe4d666987e06ee7bca4166e3cc9b296f9073d29b3d95833df755705f1fdb
# Previous ms2_12345_render.gz 7e127aa9a2e4a76415d811cdab85c71dd5c8258161c6fa115a562a0892e13d67
# Previous ms2_42_client.gz 98a01a06f8e1497c2574221f3f9bb4eed6d9a4bb0355bf34d2734344f9aa10f8
# L1 FAST re-pin (2026-10-12), prior compressed hashes retained:
# ms2_12345_client.gz 8bd1800b8dd2ed3453f02d39b3ad919889d18d9566ccf80a8f9e12110339f3a4
# ms2_12345_render.gz ea34467af55617ed4d2d1ca17160cbb32214cd9de30e827da7dbe5f22f904dba
# ms2_12345_server.gz 011aa0b893b11fbc5f30b98fbd833e6f68aeb8af36ec1966a0421f6f6858e83e
# ms2_42_client.gz a44cfd7e759e053aa0c76e2c74d97b4b8e0e8463f5e2941a5748718aba3185dd
# ms2_42_render.gz 07995fe37598101709dbe230ce272f4a590621c3c44712fadc162f613ef295f4
# ms2_42_server.gz a68911f12b9435ce252f9799b5499251e494ff20818a8d2e2a18d26162415ae4
# P reproduction + HEAD8/8: tests/golden/column_dedup/manifest.json l1_fast_repin.
# First differences inside generation setter/region changes: tick1 Sky and tick5 render.
# FINAL R6 FAST re-pin 2026-10-12: pre-R5 producer reproduces old pins;
# HEAD8/8 identical. First SERVER Sky differences tick107 (12345), tick5 (42),
# introduced by automatic load-time population (CFR ChunkProviderServer:67-95).
# Previous ms2_12345_client.gz b5a95d60c7c260c3403ebc1b0732f987ecc1cbcd72be5a9f053f16d33fec32ae
# Previous ms2_12345_server.gz a321db2cff63df72caa9775ae7e73e038c60cdd905834a8d04fae57795cf8d1b
# Previous ms2_42_client.gz c2f9ef73782f48efbf11d039658c472ef80b6e99da63af11ac3a29784c0e9e3d
# Previous ms2_42_server.gz f903a95d90b8f199b0fe5ff159ac9311e5ed21a5bba7b60ffca3c997ebe2ed1d
# Slice-2 coverage additions cannot replace or recapture the original walks.
# A second assertion beside their runtime byte comparison: literal slice-1
# SHA256s, independent of an editable capture manifest.
# Re-pinned 2026-09-27 (sixth time, lane C mob motion units S2 on 325a10e9,
# docs/intent/mob-motion-units.md; Astra rulings 2026-09-27) to the ms2 walks. The S1 tree
# 325a10e9 replays the bbdf6d6 walks exactly (test passes there); S2 moves them. First
# divergence, attributed: server stream seed 42 at server tick 17 (record 16) and seed
# 12345 at tick 575 (record 574) -- ONLY the block-id and metadata plane hashes move; the
# sky-light, block-light and heightmap planes and the resident chunk count are equal --
# i.e. a block changed by a mob-driven world callback (a mob walking or landing on a
# block: farmland trample / onEntityWalking, pressure plates) that now fires on a
# different tick because the mob's motion is Beta's (move-then-gravity, jump()
# (double)0.42F, knockBack at the hit, no engine auto-jump / ySize bank). The client
# streams follow that; the render streams keep identical sizes and mesh byte totals
# (23908016 / 27404872) with the moved cells' meshes changed. bbdf6d6 files removed
# (manifest.json keeps their hashes under previous_baseline).
# Previous pin, 2026-09-26 (fifth time, G4 assembly) to the bbdf6d6 walks: tree-heightmap
# 9afafd67 (trees read Beta's live getHeightValue, World.java:603-614) merged onto
# reed a3c02638 + growth-b 7e117359. Single-rule variants of the merged tree: tree
# reverted == 7e11735 exactly; reed + growth-b reverted == treehm's 9afafd6 exactly;
# mushroom draws reverted moves only seed-12345 state (first at server record 1151;
# the walk's only growth-b event is a brown mushroom spread at (40,63,252)).
# 7e11735 files removed (manifest.json keeps their hashes under previous_baseline).
# Previous pin, 2026-09-26 (fourth time, G4 assembly), to the 7e11735 walks: growth-b
# 7e117359 BlockMushroom.updateTick's two extra nextInt(3) [BlockMushroom.java:19-22]
# shifted the seed-12345 state walk at server tick 1644 (one brown mushroom spread
# at (34,26,296), the only growth-b event before the divergence; bisected + traced).
# Seed 42 and both render walks are byte-identical to a3c0263. a3c0263 files
# removed (manifest.json keeps their hashes under previous_baseline).
# Previous pin, 2026-09-26 (third time), to the a3c0263 walks: WORLDGEN/REED a3c02638
# (REED-SAND-DIAG-0926) ported BlockReed.canPlaceBlockAt [BlockReed.java:33-36]
# exactly -- natural sugar cane no longer generates on sand and the WorldGenReed
# spot gate is Material.water (8|9). Attributed cell by cell (old vs new
# worldgen over the three walk boxes, both seeds): every moved cell is a reed
# (83->0) or one of two spring cells shifted by the reed-occupancy height draws
# in the same chunk; render mesh bytes are identical, only the dedup generation
# numbering shifted. d2e471b files removed (manifest.json keeps their hashes
# under previous_baseline). Earlier history: 2026-09-25 d2e471b (GOLDEN-0925
# Beta corrections d8b3595d/aa8bfa1c/b0931e6e/d2e471b6), originally 624cfc2.
# FAST re-pin 2026-10-11 (maintainer ruling; integ-1011 V6 F2a/F3/F4-RNG, P=625ac64d4, 8/8):
# previous hashes 9e9e479f861c e481b603c996 fbb140e71861 d8833e7b1bb3 3bb85eefa816 3b149323b355.
set(_column_baseline_files
    ms2_12345_client.gz ms2_12345_render.gz ms2_12345_server.gz
    ms2_42_client.gz ms2_42_render.gz ms2_42_server.gz)
set(_column_baseline_hashes
    13caf710ca5f697f50d059613f19029c5f79607855d2833911ffeaaded2a7bd9
    5137639b048223c3e5f40b83370a9e22bfa031e4d63e7c3087676285e8b7d99f
    e2afa463b2f30a0466193dfb9415c6cb3852caf81e1bf81c7153f356f7b0d118
    f39fc4f83ba118b8c2b611e98f349cdcbb578d46cb9e896f9c31edfc04c897cc
    acf3f8efcb1e26ec3cd8b07e4db52b7fdb1c228309f93e7db31e72b668c909d3
    d06e0c0f345a311dc03aa4869314b8ca0665a129f3b5d2f36a2ed85436a05e93)
foreach(_i RANGE 0 5)
    list(GET _column_baseline_files ${_i} _file)
    list(GET _column_baseline_hashes ${_i} _expected)
    file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/../tests/golden/column_dedup/${_file}" _actual)
    if(NOT _actual STREQUAL _expected)
        message(FATAL_ERROR "Original column walk golden changed: ${_file}")
    endif()
endforeach()
