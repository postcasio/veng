# ValidationGate.cmake — local Vulkan validation-error gate (plan 06)
#
# Run via `cmake -P ValidationGate.cmake -D VENG_GATE_BIN_<NAME>=<path> ...`
# (see the `validation_gate` test registration in the root CMakeLists.txt).
#
# Runs each of the gpu-labelled binaries (headless_smoke, compute_dispatch,
# veng_test_gpu) and inspects their combined stdout+stderr for lines matching
# `[ERROR] Vulkan validation: ...`. Under VE_DEBUG these binaries enable the
# Vulkan validation layers; the debug messenger (engine/src/Renderer/Backend/Context.cpp)
# logs validation ERRORs via Log::Error but never aborts (CLAUDE.md), so a green
# ctest is not by itself proof of a validation-clean run. This script is that
# proof: any unallowlisted "Vulkan validation" ERROR line fails the test.
#
# Under the default (non-VE_DEBUG) build, no validation layers are enabled, so
# no such lines can appear and this gate is trivially green.

# ---------------------------------------------------------------------------
# Allowlist of documented, pinned validation gaps.
#
# Each entry is a regex matched against a single "[ERROR] Vulkan validation: ..."
# line (the message text after that prefix). A line is allowed if it matches
# ANY entry below.
#
# Empty: the storage-image UPDATE_AFTER_BIND gap (CLAUDE.md "Known validation
# gap") was closed by planset-2/06 — DescriptorSetLayout is now static by
# default (no descriptor-indexing flags unless a binding opts into
# `Bindless`), descriptorBindingStorageImageUpdateAfterBind is enabled, and the
# Primary Pool sizes every DescriptorType including StorageImage/SampledImage.
# Any "Vulkan validation" ERROR now fails the gate.
#
# (The benign MoltenVK "buffer robustness" message is logged at [WARN], not
# [ERROR], so it never reaches this allowlist — noted here for completeness
# per CLAUDE.md.)
# ---------------------------------------------------------------------------
set(VENG_VALIDATION_ALLOWLIST)

# ---------------------------------------------------------------------------
# Check one binary's combined stdout+stderr output for unallowlisted
# "[ERROR] Vulkan validation: ..." lines. Appends to VENG_GATE_FAILED (parent
# scope) if any are found.
# ---------------------------------------------------------------------------
function(veng_check_validation_output BINARY_NAME OUTPUT_TEXT)
    # The captured output is one big string containing newlines. CMake lists
    # are ';'-separated, so escape any literal ';' before turning newlines into
    # list separators with string(REPLACE), to avoid CMake misinterpreting the
    # Vulkan message text (which can itself contain ';' in some drivers/specs).
    string(REPLACE ";" "\\;" ESCAPED_TEXT "${OUTPUT_TEXT}")
    string(REPLACE "\n" ";" OUTPUT_LINES "${ESCAPED_TEXT}")

    foreach (LINE IN LISTS OUTPUT_LINES)
        # Only consider lines that are themselves a "[ERROR] Vulkan validation:"
        # message (continuation lines like "The Vulkan spec states: ..." have no
        # such prefix and are ignored).
        if (NOT LINE MATCHES "\\[ERROR\\] Vulkan validation: (.*)$")
            continue ()
        endif ()

        set(MESSAGE_TEXT "${CMAKE_MATCH_1}")

        set(ALLOWED FALSE)
        foreach (PATTERN IN LISTS VENG_VALIDATION_ALLOWLIST)
            if (MESSAGE_TEXT MATCHES "${PATTERN}")
                set(ALLOWED TRUE)
                break ()
            endif ()
        endforeach ()

        if (NOT ALLOWED)
            message(WARNING "validation_gate: ${BINARY_NAME}: unallowlisted Vulkan validation ERROR:\n  ${LINE}")
            set(VENG_GATE_FAILED TRUE PARENT_SCOPE)
        endif ()
    endforeach ()
endfunction()

# ---------------------------------------------------------------------------
# Run the gate over all configured binaries.
# ---------------------------------------------------------------------------
set(VENG_GATE_FAILED FALSE)

set(VENG_GATE_NAMES
    "veng_test_headless_smoke"
    "veng_test_compute_dispatch"
    "veng_test_gpu"
    "hello_triangle-launcher"
)
set(VENG_GATE_PATHS
    "${VENG_GATE_BIN_HEADLESS_SMOKE}"
    "${VENG_GATE_BIN_COMPUTE_DISPATCH}"
    "${VENG_GATE_BIN_GPU}"
    "${VENG_GATE_BIN_LAUNCHER}"
)
# Per-binary extra environment (empty for most). The launcher runs windowed by
# default; HT_SMOKE makes it render headless and exit, so the gate can run it
# display-free like the rest of the band. VENG_GATE_LAUNCHER_CAPTURE is the temp
# PPM path passed in by the test registration.
set(VENG_GATE_ENVS
    ""
    ""
    ""
    "HT_SMOKE=${VENG_GATE_LAUNCHER_CAPTURE}"
)

# The MaterialPreview GPU target (two live SceneRenderers) is gated on the editor
# library, so it may not exist in every build. Add it to the gate only when its path
# was passed in.
if (VENG_GATE_BIN_MATERIAL_PREVIEW)
    list(APPEND VENG_GATE_NAMES "veng_test_material_preview")
    list(APPEND VENG_GATE_PATHS "${VENG_GATE_BIN_MATERIAL_PREVIEW}")
    list(APPEND VENG_GATE_ENVS "")
endif ()

list(LENGTH VENG_GATE_NAMES VENG_GATE_COUNT)
math(EXPR VENG_GATE_LAST "${VENG_GATE_COUNT} - 1")

foreach (INDEX RANGE ${VENG_GATE_LAST})
    list(GET VENG_GATE_NAMES ${INDEX} BINARY_NAME)
    list(GET VENG_GATE_PATHS ${INDEX} BINARY_PATH)
    list(GET VENG_GATE_ENVS ${INDEX} BINARY_ENV)

    if (NOT EXISTS "${BINARY_PATH}")
        message(FATAL_ERROR "validation_gate: ${BINARY_NAME} binary not found at '${BINARY_PATH}' — build it first.")
    endif ()

    set(ENV_PREFIX)
    if (BINARY_ENV)
        set(ENV_PREFIX ${CMAKE_COMMAND} -E env ${BINARY_ENV})
    endif ()

    execute_process(
        COMMAND ${ENV_PREFIX} "${BINARY_PATH}"
        OUTPUT_VARIABLE BINARY_STDOUT
        ERROR_VARIABLE BINARY_STDERR
        RESULT_VARIABLE BINARY_RESULT
    )

    if (BINARY_RESULT EQUAL 77)
        message(STATUS "validation_gate: ${BINARY_NAME} skipped (no Vulkan ICD)")
        continue ()
    endif ()

    veng_check_validation_output("${BINARY_NAME}" "${BINARY_STDOUT}\n${BINARY_STDERR}")
endforeach ()

if (VENG_GATE_FAILED)
    message(FATAL_ERROR "validation_gate: one or more binaries emitted unallowlisted Vulkan validation ERRORs (see warnings above).")
endif ()

message(STATUS "validation_gate: no unallowlisted Vulkan validation ERRORs found.")
