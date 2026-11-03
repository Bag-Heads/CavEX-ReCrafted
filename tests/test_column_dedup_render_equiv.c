/* ci-1101/47 FAST re-pin (2026-11-02): accepted natural-spawn 211def8d8
 * alone explains this move. HEAD dee9d3edf with only that change reverted
 * reproduces both old tapes; HEAD reproduces both current render tapes.
 * First difference: seed12345 record1220/world tick1225, byte11635756,
 * dirty generation7->9. Exactly98 generation fields change; every other
 * header/list/vertex byte is identical. Seed42 stays byte-identical.
 * ci-1101/39 item10 queue fix changes neither render tape. Old/new SHA256
 * and isolated proof: manifest.json ci_1101_47_repin;
 * C:/tmp/cavex-jobs/ci-1101/47/PROOF.md. Frozen regression, not a Beta oracle.
 */
/* ci-1101/12b render proof (2026-11-01; 11aa3bfc8 pins unchanged):
 * P=354809385 passes both old tapes;
 * git bisect identifies accepted IN-005 aa1ec22fb. First true difference
 * byte83207 (reported buffer82226), tick13, mesh8/list0/vertex44: Block
 * light9->14, identical position and Sky. Accepted d2cc795ff fluid relight
 * changes 117 generation fields only (first byte1068404, 32->30).
 * Fresh render bisect d2cc795ff..6838d53a0 identifies REL-FLK cf3c657fa:
 * its direct parent af645a61b equals Q+F; cf3c657fa equals 6838d53a0,
 * both complete seeds. Retained shells continue dirty generations: exactly
 * 908 seed12345 fields change (first byte4229446/record611, 2->4), with
 * every list/vertex/other header byte equal; seed42 is wholly unchanged.
 * Direct 6bb5cb128^/6bb5cb128 captures isolate the accepted Beta fast-leaf
 * delta: seed42 byte17958/record4/y64/list0, vertices452->432; seed12345
 * unchanged. Direct a5cdacdfb and a3ef1a51c captures move neither tape.
 * 6bb5cb128 reproduces both final pins. No six-file restoration assumption.
 * Previous pins: manifest.json ci_1101_12_render_repin. Direct proof:
 * C:/tmp/cavex-jobs/ci-1101/12b/; summary ../12/PROOF.md.
 * Frozen CaveWii regression traces, not Beta oracle expectations.
 */
/* L1 FAST re-pin (2026-10-12; BRIEF-L1 keeps FAST rules): previous pins
 * ms2_12345_render.gz: 6c9c08ddd806d3fb59f8d0b4ad67c4c49b57f578e04e416a3625c169a4b29922
 * ms2_42_render.gz: 928ebb774648e727634776e9f8aa922a2834b81e9b3aa5aacb41e6db08514bef
 * reproduced by isolated P (state59199bdca; render pre-R5/R4b). HEAD8/8
 * complete decoded tapes agree in r10-column-final-repeat/manifest.json.
 * First server difference: tick1/offset8 Sky; render tick5/offset12.
 * Cause: generation retains setter stores/deferred regions instead of bulk
 * relight (CFR Chunk:90-141,208-276; MetadataChunkBlock:35-171).
 * First changed non-noop ID: flow transition at (-16,21,-4), old downstream
 * water toggled early; new World.setBlockWithNotify ordering (World:273-282).
 * Evidence: tmp/jobs/g9b/g9b3-evidence/r10-{state,render}-preimage.log,
 * r10-column-first-{difference,changing-id}.json; D:/cavewii-evidence/
 * g9b3-evidence/r10-column-final-repeat. These tapes are not Beta expectations.
 */
/* pre-V6 pin (P=625ac64d4), decoded SHA256:
 * ms2_12345_render.gz: 7a264568ce0572e65feb400ff6af4808c57dd2ac919fd3db7f85a34702e651bc
 * ms2_42_render.gz: 0bce34d2b32f285110a6b8cb60e16cf38a2c778a525bc5a1c903094507167aad
 * FAST re-pin (maintainer 2026-10-11): first divergence completion16/list4/vertex98 torch11->14 after grass normalization in source/platform/pc/displaylist.c:673 (f049fa3d8/d86e8d673/90cf80703). */
/* Copyright (c) 2026 CaveWii_v2 contributors */
#include "support/column_dedup_walk.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
static void seed_fixture(struct server_local* s) {
	/* F4-RNG: runtime World.rand and body Entity.rand are fixture inputs,
	 * distinct from the level seed and existing cosmetic/Math.random seeds. */
	jrand_set_seed(&s->worlds[0].rand, 4242);
	server_local_test_seed_body_rand(s, &s->player, 6789);
}
int main(int argc, char** argv) {
	return column_dedup_walk_main_seeded(argc, argv, 2, seed_fixture);
}
