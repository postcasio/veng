#include <Veng/UI/Widgets.h>
#include <Veng/UI/Layout.h>
#include <Veng/UI/Theme.h>

#include "Joined.h"

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace Veng::UI
{
    namespace
    {
        // ImGui takes const char*, not string_view; materialize at the call boundary.
        string AsCStr(string_view s)
        {
            return string(s);
        }

        const char* FloatFormat(const DragOptions& options)
        {
            return options.Format ? options.Format : "%.3f";
        }

        const char* IntFormat(const DragOptions& options)
        {
            return options.Format ? options.Format : "%d";
        }

        ImGuiSliderFlags DragClampFlags(const DragOptions& options)
        {
            return (options.Min || options.Max) ? ImGuiSliderFlags_AlwaysClamp
                                                : ImGuiSliderFlags_None;
        }

        // A missing bound widens to the type's extreme rather than 0: a one-sided clamp
        // (e.g. .Min = 0 with no .Max) must not collapse to the degenerate [0, 0] range,
        // which AlwaysClamp's ClampZeroRange would otherwise pin every value to 0.
        float FloatMin(const DragOptions& options)
        {
            return options.Min.value_or(std::numeric_limits<float>::lowest());
        }

        float FloatMax(const DragOptions& options)
        {
            return options.Max.value_or(std::numeric_limits<float>::max());
        }
    }

    bool Drag(string_view label, f32& v, DragOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::DragFloat(id.c_str(), &v, options.Speed, FloatMin(options), FloatMax(options),
                                FloatFormat(options), DragClampFlags(options));
    }

    bool Drag(string_view label, vec2& v, DragOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::DragFloat2(id.c_str(), glm::value_ptr(v), options.Speed, FloatMin(options),
                                 FloatMax(options), FloatFormat(options), DragClampFlags(options));
    }

    bool Drag(string_view label, vec3& v, DragOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::DragFloat3(id.c_str(), glm::value_ptr(v), options.Speed, FloatMin(options),
                                 FloatMax(options), FloatFormat(options), DragClampFlags(options));
    }

    bool Drag(string_view label, vec4& v, DragOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::DragFloat4(id.c_str(), glm::value_ptr(v), options.Speed, FloatMin(options),
                                 FloatMax(options), FloatFormat(options), DragClampFlags(options));
    }

    bool Drag(string_view label, i32& v, DragOptions options)
    {
        const string id = AsCStr(label);
        const i32 min =
            options.Min ? static_cast<i32>(*options.Min) : std::numeric_limits<i32>::min();
        const i32 max =
            options.Max ? static_cast<i32>(*options.Max) : std::numeric_limits<i32>::max();
        return ImGui::DragInt(id.c_str(), &v, options.Speed, min, max, IntFormat(options),
                              DragClampFlags(options));
    }

    bool Slider(string_view label, f32& v, SliderOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::SliderFloat(id.c_str(), &v, options.Min, options.Max, options.Format);
    }

    bool Slider(string_view label, vec2& v, SliderOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::SliderFloat2(id.c_str(), glm::value_ptr(v), options.Min, options.Max,
                                   options.Format);
    }

    bool Slider(string_view label, vec3& v, SliderOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::SliderFloat3(id.c_str(), glm::value_ptr(v), options.Min, options.Max,
                                   options.Format);
    }

    bool Slider(string_view label, vec4& v, SliderOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::SliderFloat4(id.c_str(), glm::value_ptr(v), options.Min, options.Max,
                                   options.Format);
    }

    bool Slider(string_view label, i32& v, i32 min, i32 max)
    {
        const string id = AsCStr(label);
        return ImGui::SliderInt(id.c_str(), &v, min, max);
    }

    bool Knob(string_view label, f32& v, KnobOptions options)
    {
        const Theme& theme = GetTheme();
        const string id = AsCStr(label);
        const string visible(label.substr(0, label.find("##")));

        const f32 diameter =
            options.Diameter > 0.0f ? options.Diameter : ImGui::GetFrameHeight() * 2.2f;
        const f32 range = options.Max - options.Min;
        const f32 speed =
            options.Speed > 0.0f ? options.Speed : (range > 0.0f ? range / 200.0f : 0.0f);

        ImGui::BeginGroup();
        const ImVec2 topLeft = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(id.c_str(), ImVec2(diameter, diameter));

        bool changed = false;
        if (ImGui::IsItemActive())
        {
            const f32 delta = ImGui::GetIO().MouseDelta.y;
            if (delta != 0.0f)
            {
                // Vertical drag turns the knob: up (negative screen delta) increases.
                v = std::clamp(v - delta * speed, options.Min, options.Max);
                changed = true;
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            v = (options.Min + options.Max) * 0.5f;
            changed = true;
        }
        if (ImGui::IsItemActive() || ImGui::IsItemHovered())
        {
            char readout[64];
            std::snprintf(readout, sizeof(readout), options.Format, static_cast<double>(v));
            ImGui::SetTooltip("%s", readout);
        }

        // The 270-degree sweep with its gap at the bottom: min at the lower-left, max at the
        // lower-right, turning clockwise over the top.
        const ImVec2 center(topLeft.x + diameter * 0.5f, topLeft.y + diameter * 0.5f);
        const f32 radius = diameter * 0.5f - 2.0f;
        constexpr f32 Pi = 3.14159265358979323846f;
        const f32 aMin = Pi * 0.75f;
        const f32 aMax = Pi * 2.25f;
        const f32 t = range > 0.0f ? std::clamp((v - options.Min) / range, 0.0f, 1.0f) : 0.0f;
        const f32 aVal = aMin + t * (aMax - aMin);
        const f32 thickness = std::max(2.0f, diameter * 0.09f);

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 track = ImGui::GetColorU32(SrgbToLinear(theme.SurfaceRaised));
        const ImU32 fill = ImGui::GetColorU32(
            SrgbToLinear(ImGui::IsItemActive() ? theme.AccentActive : theme.Accent));
        drawList->AddCircleFilled(center, radius - thickness * 0.5f,
                                  ImGui::GetColorU32(SrgbToLinear(theme.Surface)), 32);
        drawList->PathArcTo(center, radius, aMin, aMax, 32);
        drawList->PathStroke(track, 0, thickness);
        drawList->PathArcTo(center, radius, aMin, aVal, 32);
        drawList->PathStroke(fill, 0, thickness);
        // The indicator points from near the hub out to the value angle.
        const ImVec2 dir(std::cos(aVal), std::sin(aVal));
        drawList->AddLine(
            ImVec2(center.x + dir.x * radius * 0.35f, center.y + dir.y * radius * 0.35f),
            ImVec2(center.x + dir.x * (radius - thickness),
                   center.y + dir.y * (radius - thickness)),
            ImGui::GetColorU32(SrgbToLinear(theme.Text)), std::max(1.5f, thickness * 0.6f));

        // The label sits centered beneath the knob, in the muted caption color.
        if (!visible.empty())
        {
            const ImVec2 labelSize = ImGui::CalcTextSize(visible.c_str());
            const f32 indent = (diameter - labelSize.x) * 0.5f;
            if (indent > 0.0f)
            {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
            }
            ImGui::PushStyleColor(ImGuiCol_Text, SrgbToLinear(theme.TextMuted));
            ImGui::TextUnformatted(visible.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndGroup();
        return changed;
    }

    bool ColorEdit3(string_view label, vec3& v)
    {
        const string id = AsCStr(label);
        return ImGui::ColorEdit3(id.c_str(), glm::value_ptr(v));
    }

    bool ColorEdit4(string_view label, vec4& v)
    {
        const string id = AsCStr(label);
        return ImGui::ColorEdit4(id.c_str(), glm::value_ptr(v));
    }

    bool Checkbox(string_view label, bool& v)
    {
        const string id = AsCStr(label);
        return ImGui::Checkbox(id.c_str(), &v);
    }

    namespace
    {
        // Shared scratch-buffered text edit. Write-back happens only on Enter or
        // deactivate-after-edit. A single static scratch is safe: ImGui activates at most
        // one input item at a time, so the scratch is owned by whichever item's id matches
        // s_ActiveId. A null hint draws a plain field; a non-null hint draws the placeholder.
        // A multi-line field commits only on deactivate (Enter inserts a newline), so it
        // ignores the hint (ImGui has no multi-line hint variant).
        bool InputTextImpl(string_view label, const char* hint, string& v, bool multiline,
                           vec2 size)
        {
            const string id = AsCStr(label);

            static vector<char> s_Scratch;
            static ImGuiID s_ActiveId = 0;

            const ImGuiID itemId = ImGui::GetID(id.c_str());

            if (s_ActiveId != itemId)
            {
                s_Scratch.assign(v.begin(), v.end());
                s_Scratch.push_back('\0');
            }

            const auto resizeCallback = [](ImGuiInputTextCallbackData* data) -> int
            {
                if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
                {
                    auto* scratch = static_cast<vector<char>*>(data->UserData);
                    scratch->resize(static_cast<usize>(data->BufTextLen) + 1);
                    data->Buf = scratch->data();
                }
                return 0;
            };

            // A multi-line field reserves Enter for a newline, so it commits only on
            // deactivate-after-edit; EnterReturnsTrue applies to the single-line forms.
            const ImGuiInputTextFlags flags =
                ImGuiInputTextFlags_CallbackResize |
                (multiline ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_EnterReturnsTrue);

            // A single-line field participates in a joined group (a search box fused with a
            // trailing button); a multi-line box never appears in one.
            if (!multiline)
            {
                JoinedPreItem();
            }
            bool entered = false;
            if (multiline)
            {
                entered = ImGui::InputTextMultiline(id.c_str(), s_Scratch.data(), s_Scratch.size(),
                                                    size, flags, resizeCallback, &s_Scratch);
            }
            else if (hint != nullptr)
            {
                entered =
                    ImGui::InputTextWithHint(id.c_str(), hint, s_Scratch.data(), s_Scratch.size(),
                                             flags, resizeCallback, &s_Scratch);
            }
            else
            {
                entered = ImGui::InputText(id.c_str(), s_Scratch.data(), s_Scratch.size(), flags,
                                           resizeCallback, &s_Scratch);
            }

            if (!multiline)
            {
                JoinedPostItem();
            }

            // ImGui::IsItemActive is valid only after the widget is submitted, so the
            // scratch's owning id is updated here, after InputText.
            if (ImGui::IsItemActive())
            {
                s_ActiveId = itemId;
            }
            else if (s_ActiveId == itemId)
            {
                s_ActiveId = 0;
            }

            if (entered || ImGui::IsItemDeactivatedAfterEdit())
            {
                v = s_Scratch.data();
                return true;
            }
            return false;
        }
    }

    bool InputText(string_view label, string& v)
    {
        return InputTextImpl(label, nullptr, v, false, {});
    }

    bool InputTextWithHint(string_view label, string_view hint, string& v)
    {
        const string hintStr = AsCStr(hint);
        return InputTextImpl(label, hintStr.c_str(), v, false, {});
    }

    bool InputTextMultiline(string_view label, string& v, vec2 size)
    {
        return InputTextImpl(label, nullptr, v, true, size);
    }

    bool Combo(string_view label, i32& index, std::span<const string_view> items)
    {
        const string id = AsCStr(label);

        const string_view preview = (index >= 0 && static_cast<usize>(index) < items.size())
                                        ? items[static_cast<usize>(index)]
                                        : string_view{};
        const string previewStr(preview);

        JoinedPreItem();
        bool changed = false;
        const bool open = ImGui::BeginCombo(id.c_str(), previewStr.c_str());
        JoinedPostItem();
        if (open)
        {
            for (usize i = 0; i < items.size(); ++i)
            {
                const string item(items[i]);
                const bool selected = (static_cast<usize>(index) == i);
                if (ImGui::Selectable(item.c_str(), selected))
                {
                    index = static_cast<i32>(i);
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool Button(string_view label)
    {
        const string id = AsCStr(label);
        JoinedPreItem();
        const bool clicked = ImGui::Button(id.c_str());
        JoinedPostItem();
        return clicked;
    }

    bool SmallButton(string_view label)
    {
        const string id = AsCStr(label);
        return ImGui::SmallButton(id.c_str());
    }

    bool InvisibleButton(string_view id, vec2 size)
    {
        const string label = AsCStr(id);
        return ImGui::InvisibleButton(label.c_str(), size);
    }

    bool ToggleButton(string_view label, bool& active)
    {
        const string id = AsCStr(label);

        // Only push the accent fill while on; an off toggle reads as a plain button.
        // The authored theme colors are sRGB, linearized for the linear UI pipeline.
        if (active)
        {
            const Theme& theme = GetTheme();
            ImGui::PushStyleColor(ImGuiCol_Button, SrgbToLinear(theme.Accent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, SrgbToLinear(theme.AccentHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, SrgbToLinear(theme.AccentActive));
            ImGui::PushStyleColor(ImGuiCol_Text, SrgbToLinear(theme.TextOnAccent));
        }

        const bool clicked = ImGui::Button(id.c_str());

        if (active)
        {
            ImGui::PopStyleColor(4);
        }

        if (clicked)
        {
            active = !active;
        }
        return clicked;
    }

    namespace
    {
        // Draws one custom icon/label segment: an InvisibleButton hit area with a rounded fill
        // and centered text drawn over it, matching the Badge draw-list pattern. The fill color
        // is picked from the given rest/hovered/active family by the item's live interaction
        // state, and only the requested outer corners round (the rest square against neighbors).
        // Returns whether the segment was clicked this frame.
        bool DrawSegment(string_view id, string_view text, vec2 size, vec4 rest, vec4 hovered,
                         vec4 active, vec4 textColor, ImDrawFlags cornerFlags)
        {
            const string label(id);
            // Draw only the visible portion of the label — the id part after `##` is ImGui's
            // convention for a hidden id suffix and must not render.
            const string str(text.substr(0, text.find("##")));
            const ImVec2 pos = ImGui::GetCursorScreenPos();

            const bool clicked = ImGui::InvisibleButton(label.c_str(), size);
            const bool isHovered = ImGui::IsItemHovered();
            const bool isActive = ImGui::IsItemActive();

            const vec4 fill = isActive ? active : (isHovered ? hovered : rest);

            ImDrawList* drawList = ImGui::GetWindowDrawList();
            const ImVec2 max = ImVec2(pos.x + size.x, pos.y + size.y);
            // Authored colors are sRGB; linearize for the linear UI pipeline.
            drawList->AddRectFilled(pos, max, ImGui::GetColorU32(SrgbToLinear(fill)),
                                    GetTheme().FrameRounding, cornerFlags);

            const ImVec2 textSize = ImGui::CalcTextSize(str.c_str());
            const ImVec2 textPos = ImVec2(pos.x + ((size.x - textSize.x) * 0.5f),
                                          pos.y + ((size.y - textSize.y) * 0.5f));
            drawList->AddText(textPos, ImGui::GetColorU32(SrgbToLinear(textColor)), str.c_str());
            return clicked;
        }
    }

    bool IconButton(string_view glyph)
    {
        const Theme& theme = GetTheme();
        const f32 side = ImGui::GetFrameHeight();

        JoinedPreItem();
        const ImDrawFlags corners = JoinedCornerFlags(true, true);
        const bool clicked =
            DrawSegment(glyph, glyph, vec2(side, side), theme.SurfaceRaised, theme.SurfaceHovered,
                        theme.SurfaceActive, theme.Text, corners);
        JoinedPostItem();
        return clicked;
    }

    bool IconToggleButton(string_view glyph, bool& active)
    {
        const Theme& theme = GetTheme();
        const f32 side = ImGui::GetFrameHeight();

        // While on the button fills with the accent family and draws on-accent text; while off it
        // reads as a plain surface button, matching ToggleButton's on/off treatment.
        const vec4 rest = active ? theme.Accent : theme.SurfaceRaised;
        const vec4 hovered = active ? theme.AccentHovered : theme.SurfaceHovered;
        const vec4 pressed = active ? theme.AccentActive : theme.SurfaceActive;
        const vec4 textColor = active ? theme.TextOnAccent : theme.Text;

        JoinedPreItem();
        const ImDrawFlags corners = JoinedCornerFlags(true, true);
        const bool clicked =
            DrawSegment(glyph, glyph, vec2(side, side), rest, hovered, pressed, textColor, corners);
        JoinedPostItem();

        if (clicked)
        {
            active = !active;
        }
        return clicked;
    }

    bool ButtonGroup(string_view id, i32& index, std::span<const ButtonGroupItem> items)
    {
        const Theme& theme = GetTheme();
        const ImGuiStyle& style = ImGui::GetStyle();
        const f32 side = ImGui::GetFrameHeight();

        const string groupId = AsCStr(id);
        ImGui::PushID(groupId.c_str());

        // A group is one fused control: only its outer segments' outer edges round, and inside
        // an enclosing Joined scope JoinedCornerFlags squares the whole group.
        JoinedPreItem();

        bool changed = false;
        for (usize i = 0; i < items.size(); ++i)
        {
            const ButtonGroupItem& item = items[i];
            const string labelStr(item.Label);

            // An icon glyph sizes the segment square; a text label sizes to its text plus frame
            // padding, matching a stock button's width.
            const ImVec2 textSize = ImGui::CalcTextSize(labelStr.c_str());
            const bool iconOnly = textSize.x <= side;
            const f32 width = iconOnly ? side : textSize.x + (style.FramePadding.x * 2.0f);

            const bool isActive = static_cast<i32>(i) == index;
            const vec4 rest = isActive ? theme.Accent : theme.SurfaceRaised;
            const vec4 hovered = isActive ? theme.AccentHovered : theme.SurfaceHovered;
            const vec4 pressed = isActive ? theme.AccentActive : theme.SurfaceActive;
            const vec4 textColor = isActive ? theme.TextOnAccent : theme.Text;

            const bool roundLeft = i == 0;
            const bool roundRight = i + 1 == items.size();
            const ImDrawFlags corners = JoinedCornerFlags(roundLeft, roundRight);

            if (i > 0)
            {
                ImGui::SameLine(0.0f, 0.0f);
            }

            if (DrawSegment(fmt::format("##seg{}", i), item.Label, vec2(width, side), rest, hovered,
                            pressed, textColor, corners))
            {
                if (index != static_cast<i32>(i))
                {
                    index = static_cast<i32>(i);
                    changed = true;
                }
            }
            if (!item.Tooltip.empty() && ImGui::IsItemHovered())
            {
                const string tip(item.Tooltip);
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }

        JoinedPostItem();
        ImGui::PopID();
        return changed;
    }

    namespace
    {
        ImGuiSelectableFlags ToImGui(SelectableFlags flags)
        {
            ImGuiSelectableFlags out = ImGuiSelectableFlags_None;
            if ((flags & SelectableFlags::SpanAllColumns) != SelectableFlags::None)
            {
                out |= ImGuiSelectableFlags_SpanAllColumns;
            }
            if ((flags & SelectableFlags::AllowDoubleClick) != SelectableFlags::None)
            {
                out |= ImGuiSelectableFlags_AllowDoubleClick;
            }
            return out;
        }
    }

    bool Selectable(string_view label, bool selected, SelectableFlags flags)
    {
        const string id = AsCStr(label);
        return ImGui::Selectable(id.c_str(), selected, ToImGui(flags));
    }

    bool Selectable(string_view label, bool selected, vec2 size, SelectableFlags flags)
    {
        const string id = AsCStr(label);
        return ImGui::Selectable(id.c_str(), selected, ToImGui(flags), size);
    }

    void Text(string_view text)
    {
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
    }

    void TextDisabled(string_view text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopStyleColor();
    }

    void TextColored(vec4 color, string_view text)
    {
        // The caller's color is authored sRGB; linearize for the linear UI pipeline.
        ImGui::PushStyleColor(ImGuiCol_Text, SrgbToLinear(color));
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopStyleColor();
    }

    void Label(string_view label, string_view value)
    {
        // LabelText is printf-only; pass the string_view value via "%.*s".
        const string id = AsCStr(label);
        ImGui::LabelText(id.c_str(), "%.*s", static_cast<int>(value.size()), value.data());
    }

    void SeparatorText(string_view text)
    {
        const string label = AsCStr(text);
        ImGui::SeparatorText(label.c_str());
    }

    void TypeLabel(string_view name, string_view ns)
    {
        Text(name);
        if (!ns.empty())
        {
            SameLine();
            TextDisabled(fmt::format("({})", ns));
        }
    }

    string FormatTypeLabel(string_view name, string_view ns)
    {
        if (ns.empty())
        {
            return string(name);
        }
        return fmt::format("{} ({})", name, ns);
    }

    void Image(const Ref<ImGuiTexture>& tex, vec2 size)
    {
        ImGui::Image(static_cast<ImTextureID>(tex->GetTextureId()), size);
    }

    void PlotLines(string_view label, std::span<const f32> values, PlotOptions options)
    {
        const string id = AsCStr(label);
        const string overlay = AsCStr(options.OverlayText);

        // FLT_MAX is ImGui's sentinel for "autoscale this bound"; map nullopt onto it.
        const float scaleMin = options.ScaleMin.value_or(std::numeric_limits<float>::max());
        const float scaleMax = options.ScaleMax.value_or(std::numeric_limits<float>::max());

        ImGui::PlotLines(id.c_str(), values.data(), static_cast<int>(values.size()), options.Offset,
                         options.OverlayText.empty() ? nullptr : overlay.c_str(), scaleMin,
                         scaleMax, ImVec2(options.Size.x, options.Size.y));
    }

    void PlotLinesMulti(string_view label, std::span<const PlotSeries> series, PlotOptions options)
    {
        const Theme& theme = GetTheme();

        // Resolve the chart rect: a zero width fills the content region, a zero height takes a
        // default. The rect is reserved (Dummy) after drawing, since the draw list records at
        // absolute coordinates computed up front — the Badge/ItemBorder pattern.
        vec2 size = options.Size;
        if (size.x <= 0.0f)
        {
            size.x = ImGui::GetContentRegionAvail().x;
        }
        if (size.y <= 0.0f)
        {
            size.y = 80.0f;
        }

        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max = ImVec2(min.x + size.x, min.y + size.y);
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Framed background + border, matching the look of the single-series plots.
        drawList->AddRectFilled(min, max, ImGui::GetColorU32(SrgbToLinear(theme.Surface)),
                                theme.FrameRounding);
        drawList->AddRect(min, max, ImGui::GetColorU32(SrgbToLinear(theme.Border)),
                          theme.FrameRounding);

        // Shared Y axis: an unset max autoscales to the largest sample across every series.
        const float scaleMin = options.ScaleMin.value_or(0.0f);
        float scaleMax = options.ScaleMax.value_or(scaleMin);
        if (!options.ScaleMax.has_value())
        {
            for (const PlotSeries& s : series)
            {
                for (const f32 value : s.Values)
                {
                    scaleMax = std::max(scaleMax, value);
                }
            }
        }
        const float range = scaleMax > scaleMin ? scaleMax - scaleMin : 1.0f;

        // Inset a couple pixels so a line never sits on the border.
        constexpr float Pad = 2.0f;
        const float x0 = min.x + Pad;
        const float y1 = max.y - Pad;
        const float plotWidth = std::max((max.x - Pad) - x0, 1.0f);
        const float plotHeight = std::max(y1 - (min.y + Pad), 1.0f);

        vector<ImVec2> points;
        for (const PlotSeries& s : series)
        {
            const usize count = s.Values.size();
            if (count == 0)
            {
                continue;
            }

            points.clear();
            points.reserve(count);
            const float denom = count > 1 ? static_cast<float>(count - 1) : 1.0f;
            for (usize k = 0; k < count; k++)
            {
                const usize index = (static_cast<usize>(s.Offset) + k) % count;
                const float t = static_cast<float>(k) / denom;
                const float norm = std::clamp((s.Values[index] - scaleMin) / range, 0.0f, 1.0f);
                points.emplace_back(x0 + (t * plotWidth), y1 - (norm * plotHeight));
            }
            drawList->AddPolyline(points.data(), static_cast<int>(points.size()),
                                  ImGui::GetColorU32(SrgbToLinear(s.Color)), ImDrawFlags_None,
                                  1.5f);
        }

        if (!options.OverlayText.empty())
        {
            const string overlay = AsCStr(options.OverlayText);
            const ImVec2 textSize = ImGui::CalcTextSize(overlay.c_str());
            const ImVec2 textPos = ImVec2(min.x + ((size.x - textSize.x) * 0.5f), min.y + Pad);
            drawList->AddText(textPos, ImGui::GetColorU32(SrgbToLinear(theme.TextMuted)),
                              overlay.c_str());
        }

        // Reserve the rect as a labelled item; the draw above already recorded at these coords.
        const string id = AsCStr(label);
        ImGui::InvisibleButton(id.c_str(), size);
    }

    void ItemBorder(vec4 color, f32 thickness)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImU32 borderCol = ImGui::GetColorU32(SrgbToLinear(color));
        drawList->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), borderCol,
                          GetTheme().FrameRounding, 0, thickness);
    }

    void Badge(string_view text, vec4 color, vec2 size)
    {
        const string str = AsCStr(text);
        const ImVec2 textSize = ImGui::CalcTextSize(str.c_str());
        const ImGuiStyle& style = ImGui::GetStyle();

        vec2 badgeSize = size;
        if (badgeSize.x <= 0.0f)
        {
            badgeSize.x = textSize.x + (style.FramePadding.x * 2.0f);
        }
        if (badgeSize.y <= 0.0f)
        {
            badgeSize.y = textSize.y + (style.FramePadding.y * 2.0f);
        }

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Authored colors are sRGB; linearize for the linear UI pipeline.
        const ImU32 fillCol = ImGui::GetColorU32(SrgbToLinear(color));
        drawList->AddRectFilled(pos, ImVec2(pos.x + badgeSize.x, pos.y + badgeSize.y), fillCol,
                                GetTheme().FrameRounding);

        const ImU32 textCol = ImGui::GetColorU32(SrgbToLinear(GetTheme().TextOnAccent));
        const ImVec2 textPos = ImVec2(pos.x + ((badgeSize.x - textSize.x) * 0.5f),
                                      pos.y + ((badgeSize.y - textSize.y) * 0.5f));
        drawList->AddText(textPos, textCol, str.c_str());

        ImGui::Dummy(badgeSize);
    }

    void ProgressBar(f32 fraction, vec2 size, string_view overlay)
    {
        // The fill reads the histogram slot; theme it with the accent so the bar matches the
        // rest of the UI rather than ImGui's default.
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, SrgbToLinear(GetTheme().Accent));

        // A negative fraction is the indeterminate request; ImGui animates the sweep from the
        // value's fractional motion, so feed it the running time to keep it moving.
        const float value =
            fraction < 0.0f ? -1.0f * static_cast<float>(ImGui::GetTime()) : fraction;

        const string overlayStr = AsCStr(overlay);
        ImGui::ProgressBar(value, ImVec2(size.x, size.y),
                           overlay.empty() ? nullptr : overlayStr.c_str());

        ImGui::PopStyleColor();
    }
}
