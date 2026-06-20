#pragma once

/// @brief Public ImGui surface: the headers a UI consumer needs to build windows and node editors.
///
/// The `_internal.h` headers are deliberately not pulled in here — include
/// `<Veng/Vendor/ImGuiInternal.h>` if you genuinely need ImGui internals.
#include <imgui.h>
#include <imnodes.h>
