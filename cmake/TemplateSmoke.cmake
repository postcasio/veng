# TemplateSmoke.cmake — build-only smoke check for the template launcher.
#
# Run via `cmake -P TemplateSmoke.cmake -D VENG_SMOKE_BIN=<path> ...`
# (see the `template_launcher_smoke` test registration in the root CMakeLists.txt).
#
# Runs the template launcher headless (TEMPLATE_SMOKE), then checks the capture is the
# expected size and non-blank with template_capture_check. The template carries no pixel
# golden — this proves it builds, runs, and draws something, cheaply. The launcher +
# libtemplate + template.vengpack are the relocatable trio, so this also exercises the
# dlopen -> VengModuleRegister -> registry -> Run() shipping path end to end.
#
# Skip contract: the template has no probe of its own, so this runs veng_test_headless_smoke
# first, which exits 77 when no usable Vulkan ICD is present. On a 77 the check is skipped
# and the script succeeds (a clean pass without rendering), the same way smoke_golden treats
# a 77. The template's texture-free flat material has no ASTC dependency, so no codec probe
# is needed.

foreach (VAR VENG_SMOKE_BIN VENG_CHECK_BIN VENG_PROBE_BIN VENG_TMP VENG_WIDTH VENG_HEIGHT)
    if (NOT DEFINED ${VAR})
        message(FATAL_ERROR "template_launcher_smoke: ${VAR} not set")
    endif ()
endforeach ()

# ---- Driver probe -----------------------------------------------------------
execute_process(COMMAND "${VENG_PROBE_BIN}" RESULT_VARIABLE PROBE_RESULT
                OUTPUT_QUIET ERROR_QUIET)
if (PROBE_RESULT EQUAL 77)
    message(STATUS "template_launcher_smoke: skipped (no Vulkan ICD)")
    return ()
endif ()

# ---- Render the capture -----------------------------------------------------
if (EXISTS "${VENG_TMP}")
    file(REMOVE "${VENG_TMP}")
endif ()

execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "TEMPLATE_SMOKE=${VENG_TMP}" "${VENG_SMOKE_BIN}"
    RESULT_VARIABLE SMOKE_RESULT
)
if (NOT SMOKE_RESULT EQUAL 0)
    message(FATAL_ERROR "template_launcher_smoke: template-launcher exited ${SMOKE_RESULT}")
endif ()
if (NOT EXISTS "${VENG_TMP}")
    message(FATAL_ERROR "template_launcher_smoke: template-launcher wrote no capture to '${VENG_TMP}'")
endif ()

# ---- Size + non-blank check -------------------------------------------------
execute_process(
    COMMAND "${VENG_CHECK_BIN}" "${VENG_TMP}" "${VENG_WIDTH}" "${VENG_HEIGHT}"
    RESULT_VARIABLE CHECK_RESULT
)
if (NOT CHECK_RESULT EQUAL 0)
    message(FATAL_ERROR "template_launcher_smoke: capture failed the size/non-blank check")
endif ()

message(STATUS "template_launcher_smoke: capture is correctly sized and non-blank.")
