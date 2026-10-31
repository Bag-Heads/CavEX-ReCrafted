/* Copyright (c) 2026 CaveWii_v2 contributors */
#ifndef TESTS_COLUMN_DEDUP_WALK_H
#define TESTS_COLUMN_DEDUP_WALK_H
int column_dedup_walk_main(int argc, char** argv, unsigned trace);
/* Opt-in fixture inputs, called after each constructed walk fixture and before
 * population. The existing entry point retains its original behavior. */
struct server_local;
int column_dedup_walk_main_seeded(int argc, char** argv, unsigned trace,
	void (*seed_fixture)(struct server_local*));
#endif
