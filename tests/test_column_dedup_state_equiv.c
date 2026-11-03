/* ci-1101/47 FAST re-pin (2026-11-02): HEAD dee9d3edf with ONLY accepted
 * natural-spawn exclusion 211def8d8 reverted reproduces both old state tapes
 * and both old render tapes. 211def8d8 changes spawn acceptance/shared RNG:
 * seed12345 server tick230/byte6420 IDs+metadata; client record10615.
 * Applying ONLY ci-1101/39 item10 (Beta scheduled-tick dedup/order) to HEAD
 * reproduces every byte of both current state tapes: client IDs first change
 * at records3703/3759 (bytes133288/135304); seed42 server Block light first
 * changes at tick695/byte19452. No other batch change is needed.
 * Old/new decoded SHA256 and full toggle/capture proof: manifest.json
 * ci_1101_47_repin; C:/tmp/cavex-jobs/ci-1101/47/PROOF.md.
 * Frozen CaveWii regression streams, not Beta oracle expectations.
 */
/* ci-1101/12 FAST re-pin (2026-11-01): P=354809385 passes the old tapes;
 * all three private-history bisects identify accepted IN-005 aa1ec22fb.
 * First server difference: tick2/offset48, torch 448875->3357488978 (12345),
 * 3182550524->1050588483 (42); all other server fields remain identical.
 * Client torch first differs at record27/offset1000 and record51/offset1864.
 * Also includes accepted d2cc795ff fluid relight, cf3c657fa mesh retention,
 * and 6838d53a0 lossless client relight: both complete state tapes equal
 * 6838d53a0; two current captures repeat exactly. Previous pins and detailed
 * attribution: manifest.json ci_1101_12_state_repin; evidence
 * C:/tmp/cavex-jobs/ci-1101/12/. Frozen regression traces, not Beta oracles.
 */
/* L1 FAST re-pin (2026-10-12; BRIEF-L1 keeps FAST rules): previous pins
 * ms2_12345_client.gz: 7006f6163f334efee2e16e72dc0c9371c9e35b6b16ab895e7414eb2ac7197c87
 * ms2_12345_server.gz: 232d0f7c60f6dd6f8ca2276a44d278ec9f01763efd010c45bad92cd72b72a365
 * ms2_42_client.gz: 31457c5310b98ec310ca34fd54439e7eb38b2f1ce73ff0bbd70a14d768671792
 * ms2_42_server.gz: a4e11831ecb8d741541dfa0cc58618f75f5016d781c2325ae77c8b2e73e8ce8c
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
 * ms2_12345_server.gz: 1991c2f1fd74718cd03a0304a8455905271d6e37e82b2b4787894b62912e3415
 * ms2_12345_client.gz: 3456d699cddaac682ae19a8201fe7c7a92fa692457dd09241104888fbc734990
 * ms2_42_server.gz: 7c1580135dca97b1ca4f48d42290cb8aad68a0a1e37f25d87b84836da6cdb086
 * ms2_42_client.gz: df1bc58829e856162067aebc1afe8060f62abe9452f2e97f85eba3a438d87aea
 * FAST re-pin (maintainer 2026-10-11): first divergence client371/record370 offset13336 IDs/torch checksum (SET_BLOCK lava11) in source/network/server_world.c:2699 (f049fa3d8/d86e8d673/90cf80703). */
/* FAST R5/R6 re-pin (FINAL R6, 2026-10-12): isolated pre-R5/R4b producer
 * reproduces these previous decoded SHA256 pins; HEAD8/8 complete tapes
 * identical. First SERVER divergence is Sky at tick107/offset2984 (12345),
 * tick5/offset16 (42), introduced by normal-load automatic population in
 * server_world_populate_loaded_neighbours (CFR ChunkProviderServer:67-95).
 * Prior pins retained, never Beta lighting expectations:
 * 12345 server931051a42de21ad85bd560d04b7b73df70bf1b7497a496e10d47fafd054b0b96
 * 12345 client8b56a634fdd55c4822b4bb4f12b8e86c749e3c443cdeefb52902b217817d7fcd
 * 42 servera48c7a443fe482a5ea36d8ac26b86e26b895350c9111a5bf9d0cfec1740234f0
 * 42 client220dfd11981c60bb51cce6829c7ff3136aa79a822b8208f5b0c5c550994714f1
 * Evidence: tmp/jobs/g9b/g9b3-evidence/r7-column-repeat/manifest.json and
 * r6-preimage-state.log; original compressed pins archived there separately. */
/* Copyright (c) 2026 CaveWii_v2 contributors */
#include "support/column_dedup_walk.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
static FILE* g9b_trace;
static bool trace_population(const struct server_world_history_event* e) {
	if(g9b_trace && e->world && e->world->current_tick <= 1
		&& !strcmp(e->operation,"store") && e->y==COLUMN_IDS && e->chunk) {
		dict_server_chunks_it_t it; dict_server_chunks_it(it,e->world->chunks);
		while(!dict_server_chunks_end_p(it)) {
			const struct dict_server_chunks_pair_s* p=dict_server_chunks_cref(it);
			if(&p->value==e->chunk)
				return fprintf(g9b_trace,"ID,%lld,%llu,%d,%d,%d,%d,%d\n",
					(long long)e->world->seed,(unsigned long long)e->world->current_tick,
					S_CHUNK_X(p->key),S_CHUNK_Z(p->key),e->x,e->before_id,e->after_id)>0;
			dict_server_chunks_next(it);
		}
	}
	if(!g9b_trace || !e->world || e->world->current_tick < 100
		|| e->world->current_tick > 107)
		return true;
	if(strcmp(e->operation, "load") && strcmp(e->operation, "populate")
		&& strcmp(e->operation, "provide"))
		return true;
	return fprintf(g9b_trace, "%lld,%llu,%llu,%llu,%s,%s,%s,%d,%d\n",
		(long long)e->world->seed, (unsigned long long)e->world->current_tick,
		(unsigned long long)e->request, (unsigned long long)e->parent,
		e->operation, e->edge, e->outcome, e->x, e->z) > 0;
}
static void seed_fixture(struct server_local* s) {
	/* F4-RNG: runtime World.rand and body Entity.rand are fixture inputs,
	 * distinct from the level seed and existing cosmetic/Math.random seeds. */
	jrand_set_seed(&s->worlds[0].rand, 4242);
	server_local_test_seed_body_rand(s, &s->player, 6789);
	if(g9b_trace) server_world_test_history_hook = trace_population;
}
int main(int argc, char** argv) {
	const char* trace = getenv("CW_G9B_COLUMN_TRACE");
	if(trace) { g9b_trace = fopen(trace, "wb"); assert(g9b_trace); }
	int result = column_dedup_walk_main_seeded(argc, argv, 1, seed_fixture);
	server_world_test_history_hook = NULL;
	if(g9b_trace) { assert(fclose(g9b_trace) == 0); g9b_trace = NULL; }
	return result;
}
