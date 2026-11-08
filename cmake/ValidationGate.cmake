# ValidationGate.cmake — local validation-error gate
#
# Run via `cmake -P ValidationGate.cmake -D VENG_GATE_BIN_<NAME>=<path> ...`
# (see the `validation_gate` test registration in the root CMakeLists.txt).
#
# Runs each gpu binary it is handed and inspects its combined stdout+stderr for lines matching
# `[ERROR] Vulkan validation: ...` or `[ERROR] Metal validation: ...`. A Vulkan tree's binaries
# enable the Vulkan validation layers, whose debug messenger logs an ERROR but never aborts; a
# Metal tree's run with Metal's API and shader validation switched on (VENG_GATE_BACKEND=Metal sets
# MTL_DEBUG_LAYER, MTL_SHADER_VALIDATION and MTL_SHADER_VALIDATION_REPORT_TO_STDERR for them): API
# misuse aborts, the backend logs a submission's reported error as a Metal validation line, and a
# shader validation finding — which reaches no submission's error — is the layer's own report on
# stderr ("Invalid device store at offset ..., executing kernel function: ..."), which counts too.
# Either way a green ctest is not by itself proof of a validation-clean run; this
# script is that proof: any unallowlisted validation ERROR line fails the test.
#
# A tree without VE_VALIDATION compiles in no validation, so no such lines could appear; the gate
# is registered only where it is compiled in. A binary whose path is not passed is not run, which
# is how a tree leaves out what its backend does not run yet. VENG_GATE_GPU_ARGS is passed to the
# gpu suite (a doctest filter selecting the cases a tree runs).

# ---------------------------------------------------------------------------
# Allowlist of documented, pinned validation gaps.
#
# Each entry is a regex matched against a single "[ERROR] <backend> validation: ..."
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
# "[ERROR] Vulkan validation: ..." or "[ERROR] Metal validation: ..." lines. Appends to
# VENG_GATE_FAILED (parent scope) if any are found.
# ---------------------------------------------------------------------------
function(veng_check_validation_output BINARY_NAME OUTPUT_TEXT)
    # The captured output is one big string containing newlines. CMake lists
    # are ';'-separated, so escape any literal ';' before turning newlines into
    # list separators with string(REPLACE), to avoid CMake misinterpreting the
    # Vulkan message text (which can itself contain ';' in some drivers/specs).
    string(REPLACE ";" "\\;" ESCAPED_TEXT "${OUTPUT_TEXT}")
    string(REPLACE "\n" ";" OUTPUT_LINES "${ESCAPED_TEXT}")

    foreach (LINE IN LISTS OUTPUT_LINES)
        # Only consider lines that are themselves a "[ERROR] ... validation:"
        # message (continuation lines like "The Vulkan spec states: ..." have no
        # such prefix and are ignored).
        if (LINE MATCHES "\\[ERROR\\] (Vulkan|Metal) validation: (.*)$")
            set(MESSAGE_TEXT "${CMAKE_MATCH_2}")
        elseif (LINE MATCHES "^(Invalid .*, executing [a-z]+ function: .*)$")
            set(MESSAGE_TEXT "${CMAKE_MATCH_1}")
        else ()
            continue ()
        endif ()

        set(ALLOWED FALSE)
        foreach (PATTERN IN LISTS VENG_VALIDATION_ALLOWLIST)
            if (MESSAGE_TEXT MATCHES "${PATTERN}")
                set(ALLOWED TRUE)
                break ()
            endif ()
        endforeach ()

        if (NOT ALLOWED)
            message(WARNING "validation_gate: ${BINARY_NAME}: unallowlisted validation ERROR:\n  ${LINE}")
            set(VENG_GATE_FAILED TRUE PARENT_SCOPE)
        endif ()
    endforeach ()
endfunction()

# ---------------------------------------------------------------------------
# Run the gate over all configured binaries.
# ---------------------------------------------------------------------------
set(VENG_GATE_FAILED FALSE)

# The gate's self-test: VENG_GATE_CHECK_FILE names a recorded log, which is checked in place of
# running binaries, so a known-failing log proves the gate can fail.
if (VENG_GATE_CHECK_FILE)
    file(READ "${VENG_GATE_CHECK_FILE}" VENG_GATE_CHECK_TEXT)
    veng_check_validation_output("${VENG_GATE_CHECK_FILE}" "${VENG_GATE_CHECK_TEXT}")
    if (VENG_GATE_FAILED)
        message(FATAL_ERROR "validation_gate: the recorded log carries unallowlisted validation ERRORs.")
    endif ()
    message(STATUS "validation_gate: the recorded log is clean.")
    return ()
endif ()

set(VENG_GATE_NAMES)
set(VENG_GATE_PATHS)
set(VENG_GATE_ENVS)
set(VENG_GATE_ARGS)

# Appends one binary to the run, when its path was passed. ENV is a `|`-separated list of
# NAME=VALUE pairs; ARGS one argument. An empty one is stored as "-", since a list drops an empty
# element.
macro(veng_gate_add NAME PATH ENV ARGS)
    if (NOT "${PATH}" STREQUAL "")
        list(APPEND VENG_GATE_NAMES "${NAME}")
        list(APPEND VENG_GATE_PATHS "${PATH}")
        if ("${ENV}" STREQUAL "")
            list(APPEND VENG_GATE_ENVS "-")
        else ()
            list(APPEND VENG_GATE_ENVS "${ENV}")
        endif ()
        if ("${ARGS}" STREQUAL "")
            list(APPEND VENG_GATE_ARGS "-")
        else ()
            list(APPEND VENG_GATE_ARGS "${ARGS}")
        endif ()
    endif ()
endmacro()

# Metal's validation is switched on per process by its environment.
set(VENG_GATE_BACKEND_ENV "")
if (VENG_GATE_BACKEND STREQUAL "Metal")
    set(VENG_GATE_BACKEND_ENV
        "MTL_DEBUG_LAYER=1|MTL_SHADER_VALIDATION=1|MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1")
endif ()

veng_gate_add("veng_test_headless_smoke" "${VENG_GATE_BIN_HEADLESS_SMOKE}" "${VENG_GATE_BACKEND_ENV}" "")
veng_gate_add("veng_test_compute_dispatch" "${VENG_GATE_BIN_COMPUTE_DISPATCH}" "${VENG_GATE_BACKEND_ENV}" "")
veng_gate_add("veng_test_gpu" "${VENG_GATE_BIN_GPU}" "${VENG_GATE_BACKEND_ENV}" "${VENG_GATE_GPU_ARGS}")
# The launcher runs windowed by default; HT_SMOKE makes it render headless and exit, so the gate can
# run it display-free like the rest of the band.
if (NOT "${VENG_GATE_BIN_LAUNCHER}" STREQUAL "")
    set(VENG_GATE_LAUNCHER_ENV "HT_SMOKE=${VENG_GATE_LAUNCHER_CAPTURE}")
    if (NOT VENG_GATE_BACKEND_ENV STREQUAL "")
        string(APPEND VENG_GATE_LAUNCHER_ENV "|${VENG_GATE_BACKEND_ENV}")
    endif ()
    veng_gate_add("hello_triangle-launcher" "${VENG_GATE_BIN_LAUNCHER}" "${VENG_GATE_LAUNCHER_ENV}" "")
endif ()
# The MaterialPreview GPU target (two live SceneRenderers) is gated on the editor library, so it may
# not exist in every build.
veng_gate_add("veng_test_material_preview" "${VENG_GATE_BIN_MATERIAL_PREVIEW}" "${VENG_GATE_BACKEND_ENV}" "")

list(LENGTH VENG_GATE_NAMES VENG_GATE_COUNT)
if (VENG_GATE_COUNT EQUAL 0)
    message(FATAL_ERROR "validation_gate: no binary to run was passed.")
endif ()
math(EXPR VENG_GATE_LAST "${VENG_GATE_COUNT} - 1")

foreach (INDEX RANGE ${VENG_GATE_LAST})
    list(GET VENG_GATE_NAMES ${INDEX} BINARY_NAME)
    list(GET VENG_GATE_PATHS ${INDEX} BINARY_PATH)
    list(GET VENG_GATE_ENVS ${INDEX} BINARY_ENV)
    list(GET VENG_GATE_ARGS ${INDEX} BINARY_ARGS)

    if (NOT EXISTS "${BINARY_PATH}")
        message(FATAL_ERROR "validation_gate: ${BINARY_NAME} binary not found at '${BINARY_PATH}' — build it first.")
    endif ()

    set(ENV_PREFIX)
    if (NOT BINARY_ENV STREQUAL "-")
        string(REPLACE "|" ";" BINARY_ENV_LIST "${BINARY_ENV}")
        set(ENV_PREFIX ${CMAKE_COMMAND} -E env ${BINARY_ENV_LIST})
    endif ()

    set(ARGUMENTS)
    if (NOT BINARY_ARGS STREQUAL "-")
        set(ARGUMENTS "${BINARY_ARGS}")
    endif ()

    execute_process(
        COMMAND ${ENV_PREFIX} "${BINARY_PATH}" ${ARGUMENTS}
        OUTPUT_VARIABLE BINARY_STDOUT
        ERROR_VARIABLE BINARY_STDERR
        RESULT_VARIABLE BINARY_RESULT
    )

    if (BINARY_RESULT EQUAL 77)
        message(STATUS "validation_gate: ${BINARY_NAME} skipped (no GPU device)")
        continue ()
    endif ()

    # Metal's API validation reports misuse by aborting the process.
    if (VENG_GATE_BACKEND STREQUAL "Metal" AND NOT BINARY_RESULT EQUAL 0)
        message(WARNING "validation_gate: ${BINARY_NAME} exited with ${BINARY_RESULT} under Metal validation")
        set(VENG_GATE_FAILED TRUE)
    endif ()

    veng_check_validation_output("${BINARY_NAME}" "${BINARY_STDOUT}\n${BINARY_STDERR}")
endforeach ()

if (VENG_GATE_FAILED)
    message(FATAL_ERROR "validation_gate: one or more binaries emitted unallowlisted validation ERRORs (see warnings above).")
endif ()

message(STATUS "validation_gate: no unallowlisted validation ERRORs found.")
