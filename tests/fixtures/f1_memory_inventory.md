# F1 local movement credits: Wii memory inventory

Moved here from the private deviation record ("Local movement credits: bounded-overload
owner-tick suspension", V6 F1 round 4) so the public tree keeps the disclosure that
`tests/test_f1_memory_inventory.py` checks.

<!-- F1_MOVEMENT_CREDITS_2026_10_10 -->

**Wii memory inventory (compile-only, round 4).** A devkitPPC GCC 15.2.0 probe compiled with the Wii EABI (`-mcpu=750 -meabi -mhard-float`, HW_RVL) measured the actual private transport types/storage. No Wii executable was linked or run. `server_rpc` is 112 B, alignment 8; an actor event is 128 B, alignment 8. The F1 ring has **203** entries: payloads 22,736 B and sources 3,248 B, each alignment 8. Before F1 (`ea3815d75`) it had 75 entries (8,400 + 1,200 B): the bounded ring increase is 16,384 B. Round 3 reused four motion entries for controls; round 4 adds no further entries.

- The control flag array is **203 B**, alignment 1, not 85 B. Its two per-owner census values add 8 B, alignment 4. The existing two-value lifecycle census stays 8 B (no delta); the admission callback pointer adds 4 B, alignment 4.
- The credit `thread_mutex` is 8 B, alignment 4. Credit counters/identity/dispatch storage: ready 1, epoch 4, generations 16, outstanding 8, unpublished 8, host lifetime 8, stale releases 4, dispatch pointer 4, taken 1 B (62 B including the mutex). The epoch's 4 B are initialized data; the other 58 B are BSS.
- Motion census/refusal/mode add 8 + 4 + 1 B. Pause context/cancellation add 64 + 4 + 4 B. Thus transport BSS symbol sizes grow by 16,742 B, plus 4 B data. The compile-only object's actual aligned `.bss` grows 12,104→28,856 B (+16,752 B); `.sbss` is unchanged at 4 B.
- Client counters add 8 B backpressure + two 16 B tick-stat records + two 1 B reservation states = 42 B BSS (alignment 4 for counters, 1 for reservation states). Round 4 reuses those reservation bytes to signal actual physics, with no extra counter/flag allocation.
- `motion_credit_n/slot/epoch/gen` occupy 16 B per body, alignment 8 (offsets 13,344→13,360). `server_player` grows 13,736→13,752 B. The static main server's embedded host adds 16 B BSS; the optional guest's existing allocation request adds 16 B heap. Actor-event and RPC sizes remain 128/112 B, so their credit flags/tags grow neither existing array nor inbox payload.

Total F1 declared BSS-object increase: **16,800 B**, plus **4 B initialized data** and **16 B requested guest heap** when admitted; no new heap queue or allocation. Inter-object/linker alignment is separate (the measured transport section has 10 B extra padding). Round 4's production storage delta against `5fc12b623` is **zero BSS/data/heap bytes**. These compiler sizes establish the inventory, not a linked-release address/sd0/boot or real-Wii memory/feel acceptance; those remain hardware-unconfirmed. Probe and raw symbol/section receipts are in `tmp/f1r4/memory-wii.json`, `memory-compare.json` and the probe objects.
