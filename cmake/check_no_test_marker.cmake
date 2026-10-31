# Link-time hard boundary (docs/build_architecture.md step 4): a playable
# executable must never contain engine objects compiled with test
# instrumentation. Every such object set embeds the string below (see
# tests/support/test_engine_marker.c, compiled only into cavex_test_core);
# finding it in the given binary fails the build.
# Usage: cmake -DBINARY=<path> -P cmake/check_no_test_marker.cmake
if(NOT EXISTS "${BINARY}")
	message(FATAL_ERROR "test-marker gate: binary not found: ${BINARY}")
endif()
file(STRINGS "${BINARY}" _cw_marker_hits REGEX "CaveWiiTestEngineMarker")
if(_cw_marker_hits)
	message(FATAL_ERROR
		"test-marker gate FAIL: ${BINARY} contains test-instrumented engine "
		"objects (CaveWiiTestEngineMarker found). A playable executable must "
		"never link cavex_test_core objects or be compiled with "
		"CAVEWII_SERVER_LOCAL_TEST / CAVEWII_CLIENT_INTERFACE_TEST.")
endif()
message(STATUS "test-marker gate OK: no test-instrumented objects in ${BINARY}")
