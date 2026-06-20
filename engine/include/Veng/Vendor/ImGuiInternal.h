#pragma once

/// @brief ImGui/imnodes internals — opt-in.
///
/// Most consumers want `<Veng/Vendor/ImGui.h>`; reach for this only when you need internal
/// APIs (custom widgets, direct context manipulation, imnodes internals).
#include <Veng/Vendor/ImGui.h>
#include <imgui_internal.h>
#include <imnodes_internal.h>
