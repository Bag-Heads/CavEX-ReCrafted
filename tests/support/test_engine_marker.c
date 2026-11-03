/* Link-time test-marker (docs/build_architecture.md step 4 hard boundary).
 *
 * This file is compiled ONLY into cavex_test_core (the shared test engine
 * pool) and exists so every binary containing test-instrumented engine
 * objects carries a detectable symbol. Three independent gates key on it:
 *   - configure time: the playable-target boundary sweep in CMakeLists
 *     rejects CAVEWII_TESTING / cavex_test_* links on playable targets;
 *   - post-link (PC): cmake/check_no_test_marker.cmake fails the cavex app
 *     build if the marker string appears in CavEX-ReCrafted.exe;
 *   - post-link (Wii): the Makefile .dol rule readelf-greps for the symbol.
 * Do not move this into an engine source file and do not remove it: without
 * the marker, "test objects never link into playable executables" is
 * unenforced. */
#ifndef CAVEWII_TESTING
#error "test_engine_marker.c is test-pool-only; a non-test target pulled it in"
#endif

const char cavewii_test_engine_marker[] = "CaveWiiTestEngineMarker";
