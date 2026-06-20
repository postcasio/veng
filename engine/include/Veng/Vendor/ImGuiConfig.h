#pragma once

/// @brief ImGui user-config (wired as `IMGUI_USER_CONFIG`).
///
/// Injects implicit conversions between ImGui's `ImVec2`/`ImVec4` and glm's `vec2`/`vec4`
/// so the two cross freely at every imgui boundary — a call site passes a `vec2` where ImGui
/// wants an `ImVec2`, and reads an `ImVec2` return straight into a `vec2`.
///
/// Defined globally via a PUBLIC compile definition so imgui's own aggregation TU
/// (`engine/src/Vendor/ImGui.cpp`) and every consumer compile one identical `ImVec2`/`ImVec4`
/// — the macros add only member functions, no data members, so the layout is unchanged
/// regardless. glm must be complete here: `imgui.h` pulls this in before it defines `ImVec2`.

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#define IM_VEC2_CLASS_EXTRA                                  \
    ImVec2(const glm::vec2& v) : x(v.x), y(v.y) {}           \
    operator glm::vec2() const { return glm::vec2(x, y); }

#define IM_VEC4_CLASS_EXTRA                                          \
    ImVec4(const glm::vec4& v) : x(v.x), y(v.y), z(v.z), w(v.w) {}   \
    operator glm::vec4() const { return glm::vec4(x, y, z, w); }
