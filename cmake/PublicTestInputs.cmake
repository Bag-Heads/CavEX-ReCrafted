# The public export omits these private inputs. Keep the tests registered and
# report each exclusion, and automatically enable it when its input is present.
function(cavex_private_test_input test input)
    if(TEST ${test} AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${input}")
        set_tests_properties(${test} PROPERTIES DISABLED TRUE)
        set_property(TEST ${test} APPEND PROPERTY LABELS missing_private_input)
        message(STATUS "${test}: excluded -- missing private input ${input}")
    endif()
endfunction()

# Private maintainer hooks are not part of the public export.
cavex_private_test_input(test_hardware_proof_nudge .claude/hooks/hardware-proof-nudge.py)
cavex_private_test_input(test_require_hw_proof .claude/hooks/require-hw-proof.py)
cavex_private_test_input(test_no_blanket_add .claude/hooks/no-blanket-add.py)
cavex_private_test_input(test_guard_ip .claude/hooks/guard-ip.sh)
# Private original-JAR comparison implementation and oracle snapshots.
cavex_private_test_input(test_fastcmp_capture tests/oracle/lighting/g9_option_b/compare/first_divergence.py)
cavex_private_test_input(test_light_no_downgrade tests/oracle/lighting/g9_option_b/fixtures/no-downgrade/original-jar.bin.gz)
# Private original-Beta McRegion save (tests/fixtures/beta_import/README.md).
foreach(test test_convert_only test_dropin_beta_import test_dropin_heap_budget test_beta_import_lighting test_wii_telemetry_world_copy)
    cavex_private_test_input(${test} tests/fixtures/beta_import/world/level.dat)
endforeach()
# Private approved startup animation source frames.
cavex_private_test_input(test_startup_credits docs/ui_design/startup-credits-prototype/frames-B2-taps/frame_0.png)
# Private art-source ramps and complete Earth & Ash source pack.
cavex_private_test_input(test_anim_pack_colours artwork/fx-ramps/earth-and-ash/lava.ramp)
cavex_private_test_input(test_anim_fire_strip_fallback artwork/cc0-pack/earth-and-ash-complete/anim.png)
cavex_private_test_input(test_make_release artwork/cc0-pack/earth-and-ash-complete/manifest.json)
# Private approved GameCube glyph source artwork (the shipped atlases remain public).
cavex_private_test_input(test_gc_glyph_style_c artwork/controller-glyphs/gamecube-style-c/32-A.png)
