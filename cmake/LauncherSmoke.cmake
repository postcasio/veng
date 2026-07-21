# LauncherSmoke.cmake — headless launcher smoke for hello-triangle.
#
# Run via `cmake -P LauncherSmoke.cmake -D VENG_LAUNCHER_BIN=<path> ...`
# (see the `hello_triangle_launcher_smoke` test registration in the root CMakeLists.txt).
#
# Runs hello_triangle-launcher with HT_SMOKE pointed at a temp PPM (so it renders
# headless and exits) and asserts exit 0. This is the one test covering the real
# dlopen -> VengModuleRegister -> registry -> Run() binary path end-to-end.
#
# Skip contract: the launcher is the shipping binary and carries no driver probe —
# 77 is a CTest convention with no place in a game executable, and without an ICD
# the launcher aborts inside Context creation rather than exiting any particular
# code. So this script probes first, exactly as SmokeGolden.cmake does for the same
# launcher: veng_test_headless_smoke exits 77 when no usable Vulkan ICD is present,
# and on that the smoke is skipped. The test's SKIP_REGULAR_EXPRESSION matches the
# message below, so a skipped run reports as skipped rather than as passed.

foreach (VAR VENG_LAUNCHER_BIN VENG_PROBE_BIN VENG_CAPTURE)
    if (NOT DEFINED ${VAR})
        message(FATAL_ERROR "hello_triangle_launcher_smoke: ${VAR} not set")
    endif ()
endforeach ()

# ---- Driver probe -----------------------------------------------------------
execute_process(COMMAND "${VENG_PROBE_BIN}" RESULT_VARIABLE PROBE_RESULT
                OUTPUT_QUIET ERROR_QUIET)
if (PROBE_RESULT EQUAL 77)
    message(STATUS "hello_triangle_launcher_smoke: skipped (no Vulkan ICD)")
    return ()
endif ()

# ---- Run the launcher headless ----------------------------------------------
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "HT_SMOKE=${VENG_CAPTURE}" "${VENG_LAUNCHER_BIN}"
    RESULT_VARIABLE LAUNCHER_RESULT
)
if (NOT LAUNCHER_RESULT EQUAL 0)
    message(FATAL_ERROR "hello_triangle_launcher_smoke: launcher exited ${LAUNCHER_RESULT}")
endif ()

message(STATUS "hello_triangle_launcher_smoke: launcher ran headless and exited 0.")
