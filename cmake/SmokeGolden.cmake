# SmokeGolden.cmake — golden-image check for the hello-triangle smoke capture.
#
# Run via `cmake -P SmokeGolden.cmake -D VENG_SMOKE_BIN=<path> ...`
# (see the `smoke_golden` test registration in the root CMakeLists.txt).
#
# Renders the hello-triangle scene headless to a temp PPM (HT_SMOKE), then
# compares it against the checked-in golden with veng_test_golden_compare. Smoke mode
# renders a fixed pose (HelloTriangleApp::SmokeAngle), so the capture is
# reproducible run to run and a fuzzy compare catches render regressions.
#
# Skip contract: hello_triangle has no Vulkan-ICD probe of its own, so this
# script runs veng_test_headless_smoke first purely as a probe — if it exits 77 (no
# usable ICD) the golden check is skipped and the script succeeds, the same way
# validation_gate treats a 77 from its binaries.

foreach (VAR VENG_SMOKE_BIN VENG_COMPARE_BIN VENG_PROBE_BIN VENG_GOLDEN VENG_TMP)
    if (NOT DEFINED ${VAR})
        message(FATAL_ERROR "smoke_golden: ${VAR} not set")
    endif ()
endforeach ()

# ---- Driver probe -----------------------------------------------------------
execute_process(COMMAND "${VENG_PROBE_BIN}" RESULT_VARIABLE PROBE_RESULT
                OUTPUT_QUIET ERROR_QUIET)
if (PROBE_RESULT EQUAL 77)
    message(STATUS "smoke_golden: skipped (no Vulkan ICD)")
    return ()
endif ()

# ---- Render the capture -----------------------------------------------------
if (EXISTS "${VENG_TMP}")
    file(REMOVE "${VENG_TMP}")
endif ()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "HT_SMOKE=${VENG_TMP}" "${VENG_SMOKE_BIN}"
    RESULT_VARIABLE SMOKE_RESULT
)
if (NOT SMOKE_RESULT EQUAL 0)
    message(FATAL_ERROR "smoke_golden: hello_triangle exited ${SMOKE_RESULT}")
endif ()
if (NOT EXISTS "${VENG_TMP}")
    message(FATAL_ERROR "smoke_golden: hello_triangle wrote no capture to '${VENG_TMP}'")
endif ()

# ---- Compare against the golden --------------------------------------------
execute_process(
    COMMAND "${VENG_COMPARE_BIN}" "${VENG_TMP}" "${VENG_GOLDEN}"
    RESULT_VARIABLE COMPARE_RESULT
)
if (NOT COMPARE_RESULT EQUAL 0)
    message(FATAL_ERROR "smoke_golden: capture does not match golden '${VENG_GOLDEN}'")
endif ()

message(STATUS "smoke_golden: capture matches golden.")
