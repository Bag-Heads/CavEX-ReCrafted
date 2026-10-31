#include "../source/game/client_world_context.h"

#include <stdio.h>

#define CHECK(expr) do { if(!(expr)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
	return 1; \
} } while(0)

int main(void) {
	struct client_world_context overworld, nether;
	CHECK(client_ctx_init(&overworld, 0, 0));
	CHECK(client_ctx_init(&nether, -1, 1));
	CHECK(!client_ctx_init(&nether, -1, 2));
	CHECK(overworld.generation != 0);
	CHECK(client_ctx_accept(&overworld, 0, overworld.generation));
	CHECK(!client_ctx_try_free(&overworld));
	CHECK(!client_ctx_accept(&overworld, 1, overworld.generation));
	CHECK(!client_ctx_accept(&nether, 0, nether.generation));

	uint32_t old_generation = overworld.generation;
	CHECK(client_ctx_reset(&overworld));
	CHECK(overworld.generation != old_generation);
	CHECK(!client_ctx_accept(&overworld, 0, old_generation));
	CHECK(client_ctx_accept(&overworld, 0, overworld.generation));

	CHECK(client_ctx_bind_view(&overworld, 0));
	CHECK(client_ctx_bind_view(&overworld, 1));
	CHECK(overworld.bound_views == 3);
	CHECK(client_ctx_reset(&overworld));
	CHECK(overworld.bound_views == 3);
	CHECK(!client_ctx_bind_view(&overworld, 1));
	CHECK(!client_ctx_bind_view(&overworld, 2));
	CHECK(client_ctx_unbind_view(&overworld, 1));
	CHECK(client_ctx_bind_view(&nether, 1));
	CHECK(overworld.bound_views == 1 && nether.bound_views == 2);
	CHECK(!client_ctx_unbind_view(&overworld, 1));

	CHECK(client_ctx_lease_acquire(&overworld, CLIENT_CTX_LEASE_MESHER));
	CHECK(client_ctx_lease_acquire(&overworld, CLIENT_CTX_LEASE_GPU));
	CHECK(client_ctx_lease_acquire(&nether, CLIENT_CTX_LEASE_MESHER));
	uint32_t nether_generation = nether.generation;
	CHECK(client_ctx_reset(&overworld));
	CHECK(nether.generation == nether_generation);
	CHECK(nether.leases[CLIENT_CTX_LEASE_MESHER] == 1);
	CHECK(overworld.leases[CLIENT_CTX_LEASE_MESHER] == 1);
	CHECK(!client_ctx_lease_release(&overworld, CLIENT_CTX_LEASE_COUNT));

	client_ctx_retire(&overworld);
	CHECK(!client_ctx_accept(&overworld, 0, overworld.generation));
	CHECK(!client_ctx_bind_view(&overworld, 1));
	CHECK(!client_ctx_lease_acquire(&overworld, CLIENT_CTX_LEASE_GPU));
	CHECK(client_ctx_unbind_view(&overworld, 0));
	CHECK(!client_ctx_try_free(&overworld));
	CHECK(client_ctx_lease_release(&overworld, CLIENT_CTX_LEASE_MESHER));
	CHECK(!client_ctx_try_free(&overworld));
	CHECK(client_ctx_lease_release(&overworld, CLIENT_CTX_LEASE_GPU));
	CHECK(!client_ctx_lease_release(&overworld, CLIENT_CTX_LEASE_GPU));
	CHECK(overworld.leases[CLIENT_CTX_LEASE_GPU] == 0);
	CHECK(client_ctx_try_free(&overworld));
	CHECK(nether.leases[CLIENT_CTX_LEASE_MESHER] == 1);
	CHECK(client_ctx_unbind_view(&nether, 1));
	client_ctx_retire(&nether);
	CHECK(!client_ctx_try_free(&nether));
	CHECK(client_ctx_lease_release(&nether, CLIENT_CTX_LEASE_MESHER));
	CHECK(client_ctx_try_free(&nether));
	puts("PASS client context routing and lifetime");
	return 0;
}
