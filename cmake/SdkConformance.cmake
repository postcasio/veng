# SdkConformance.cmake — out-of-tree find_package(veng) consumer conformance.
#
# Run via `cmake -P SdkConformance.cmake -D VENG_MODE=<install|buildtree> ...`
# (see the sdk_conformance_install / sdk_conformance_buildtree test registrations in the
# root CMakeLists.txt). One driver parameterized on the discovery mode.
#
# It proves the SDK's exported surface is real by building a source-tree-free consumer: it
# discovers veng as a CMake package (an install prefix, or the build-tree export), then
# configures + builds the standalone examples/template against it. The configure + build
# succeeding is the core assertion — it exercises the installed/exported vengc cook, the
# veng_add_project / veng_add_game helpers, the imported-target aliases, and the mode-resolved
# path vars; a broken install/export fails the configure or the build, failing the test. It
# then asserts the launcher's relocatable set landed beside the launcher, runs the launcher
# itself headless and checks both its exit status and the marker it logs once its world resolved,
# and runs the installed veng-editor --version to exit 0 (covering the editor exe's own
# INSTALL_RPATH resolution).
#
# Modes:
#   install   — cmake --install the engine build to a throwaway prefix, then configure the
#               template with -DCMAKE_PREFIX_PATH=<prefix>. Uses the prefix's bin/veng-editor.
#   buildtree — configure the template with -Dveng_ROOT=<engine build dir>, no install step.
#               Uses the build tree's veng-editor.
#
# Skip contract: the tests run in the gpu band. This script runs the driver ICD probe first
# and returns 77 (ctest reports skipped, via SKIP_RETURN_CODE) with no usable Vulkan ICD, the
# same skip the other launcher smokes take.

foreach (VAR VENG_MODE VENG_CMAKE VENG_ENGINE_BUILD_DIR VENG_TEMPLATE_SRC VENG_SCRATCH VENG_PROBE_BIN)
    if (NOT DEFINED ${VAR})
        message(FATAL_ERROR "sdk_conformance: ${VAR} not set")
    endif ()
endforeach ()

# ---- Driver probe -----------------------------------------------------------
# Keep the two out-of-tree tests in the same skip band as the in-tree launcher smoke.
execute_process(COMMAND "${VENG_PROBE_BIN}" RESULT_VARIABLE PROBE_RESULT
                OUTPUT_QUIET ERROR_QUIET)
if (PROBE_RESULT EQUAL 77)
    message(STATUS "sdk_conformance(${VENG_MODE}): skipped (no Vulkan ICD)")
    return ()
endif ()

# ---- Fresh scratch ----------------------------------------------------------
# A clean scratch each run: the standalone configure writes a gitignored .veng/build.json
# sidecar into the template dir, so the template is copied out here rather than configured in
# place — nothing is written into the source tree.
if (EXISTS "${VENG_SCRATCH}")
    file(REMOVE_RECURSE "${VENG_SCRATCH}")
endif ()
file(MAKE_DIRECTORY "${VENG_SCRATCH}")

set(TEMPLATE_COPY "${VENG_SCRATCH}/template")
set(BUILD_DIR "${VENG_SCRATCH}/build")
file(COPY "${VENG_TEMPLATE_SRC}/" DESTINATION "${TEMPLATE_COPY}")
# Drop any sidecar carried in from a prior in-place configure of the source template.
file(REMOVE_RECURSE "${TEMPLATE_COPY}/.veng")

# ---- Discovery arg per mode -------------------------------------------------
if (VENG_MODE STREQUAL "install")
    set(PREFIX "${VENG_SCRATCH}/prefix")
    execute_process(
        COMMAND "${VENG_CMAKE}" --install "${VENG_ENGINE_BUILD_DIR}" --prefix "${PREFIX}"
        RESULT_VARIABLE INSTALL_RESULT
    )
    if (NOT INSTALL_RESULT EQUAL 0)
        message(FATAL_ERROR "sdk_conformance(install): install to '${PREFIX}' failed (${INSTALL_RESULT})")
    endif ()
    set(DISCOVERY_ARG "-DCMAKE_PREFIX_PATH=${PREFIX}")
    set(EDITOR_BIN "${PREFIX}/bin/veng-editor")
elseif (VENG_MODE STREQUAL "buildtree")
    set(DISCOVERY_ARG "-Dveng_ROOT=${VENG_ENGINE_BUILD_DIR}")
    set(EDITOR_BIN "${VENG_ENGINE_BUILD_DIR}/editor/veng-editor")
else ()
    message(FATAL_ERROR "sdk_conformance: unknown VENG_MODE '${VENG_MODE}'")
endif ()

# ---- Configure the standalone template --------------------------------------
# VENG_OSX_ARCH (Apple hosts) pins the template to the architecture the SDK libraries carry:
# a default configure follows the invoking process tree's CPU preference, which under a
# Rosetta-translated ancestor makes universal clang target x86_64 against an arm64 SDK.
set(ARCH_ARGS "")
if (DEFINED VENG_OSX_ARCH AND NOT VENG_OSX_ARCH STREQUAL "")
    list(APPEND ARCH_ARGS "-DCMAKE_OSX_ARCHITECTURES=${VENG_OSX_ARCH}")
endif ()
execute_process(
    COMMAND "${VENG_CMAKE}" -B "${BUILD_DIR}" -S "${TEMPLATE_COPY}" "${DISCOVERY_ARG}" ${ARCH_ARGS}
    RESULT_VARIABLE CONFIGURE_RESULT
)
if (NOT CONFIGURE_RESULT EQUAL 0)
    message(FATAL_ERROR "sdk_conformance(${VENG_MODE}): standalone configure failed (${CONFIGURE_RESULT})")
endif ()

# ---- Build the launcher (cook + pack pulled in as deps) ----------------------
# Nested-in-ctest build capped at -j 4 per the repo parallelism convention.
execute_process(
    COMMAND "${VENG_CMAKE}" --build "${BUILD_DIR}" -j 4 --target template-launcher
    RESULT_VARIABLE BUILD_RESULT
)
if (NOT BUILD_RESULT EQUAL 0)
    message(FATAL_ERROR "sdk_conformance(${VENG_MODE}): standalone build failed (${BUILD_RESULT})")
endif ()

# ---- Assert the relocatable set landed beside the launcher ------------------
# The launcher takes no custom RUNTIME_OUTPUT_DIRECTORY, so it and its copied module + cooked
# project + pack land in the build-dir root (the relocatable directory a ship copies whole).
if (WIN32)
    set(LAUNCHER "${BUILD_DIR}/template-launcher.exe")
    set(MODULE_LIB "${BUILD_DIR}/template.dll")
elseif (APPLE)
    set(LAUNCHER "${BUILD_DIR}/template-launcher")
    set(MODULE_LIB "${BUILD_DIR}/libtemplate.dylib")
else ()
    set(LAUNCHER "${BUILD_DIR}/template-launcher")
    set(MODULE_LIB "${BUILD_DIR}/libtemplate.so")
endif ()

foreach (ARTIFACT
        "${LAUNCHER}"
        "${MODULE_LIB}"
        "${BUILD_DIR}/project.vengproj"
        "${BUILD_DIR}/template.vengpack")
    if (NOT EXISTS "${ARTIFACT}")
        message(FATAL_ERROR "sdk_conformance(${VENG_MODE}): relocatable-set artifact missing: '${ARTIFACT}'")
    endif ()
endforeach ()

# ---- Run the launcher --------------------------------------------------------
# The build proves the SDK's *build* surface; this proves the artifacts it produced actually
# run. TEMPLATE_SMOKE makes the app windowless, tick a fixed handful of frames, and exit — so
# the run is a CI-safe assertion rather than an interactive session.
#
# The template renders no golden, so "ran correctly" is defined as: exit status 0, plus the
# marker line the app logs once its world is loaded. That line is emitted only after the
# prefab-authored AssetHandle<MarkerSet> resolved and was rehydrated, so it witnesses the whole
# custom-asset-type seam — cook module, runtime module, registered handle mapping, module
# loader — through the shipped launcher path. An exit-code-only check would pass on an app that
# started and quit having loaded nothing.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "TEMPLATE_SMOKE=1" "${LAUNCHER}"
    WORKING_DIRECTORY "${BUILD_DIR}"
    RESULT_VARIABLE LAUNCHER_RESULT
    OUTPUT_VARIABLE LAUNCHER_OUTPUT
    ERROR_VARIABLE LAUNCHER_ERROR
)
message(STATUS "sdk_conformance(${VENG_MODE}): launcher output:\n${LAUNCHER_OUTPUT}${LAUNCHER_ERROR}")
if (NOT LAUNCHER_RESULT EQUAL 0)
    message(FATAL_ERROR "sdk_conformance(${VENG_MODE}): template-launcher exited ${LAUNCHER_RESULT}")
endif ()
if (NOT LAUNCHER_OUTPUT MATCHES "MarkerBeacon resolved")
    message(FATAL_ERROR
        "sdk_conformance(${VENG_MODE}): template-launcher exited 0 but never resolved its "
        "prefab-authored game-defined asset")
endif ()

# ---- Smoke the editor exe's own runtime resolution --------------------------
# --version prints and exits before any window or device, so it covers the editor exe's
# INSTALL_RPATH resolution (libveng_editor / libveng_graph / Slang) with no project or GPU.
if (NOT EXISTS "${EDITOR_BIN}")
    message(FATAL_ERROR "sdk_conformance(${VENG_MODE}): veng-editor not found at '${EDITOR_BIN}'")
endif ()
execute_process(
    COMMAND "${EDITOR_BIN}" --version
    RESULT_VARIABLE EDITOR_RESULT
)
if (NOT EDITOR_RESULT EQUAL 0)
    message(FATAL_ERROR "sdk_conformance(${VENG_MODE}): veng-editor --version exited ${EDITOR_RESULT}")
endif ()

message(STATUS "sdk_conformance(${VENG_MODE}): out-of-tree template built, launcher and editor ran.")
