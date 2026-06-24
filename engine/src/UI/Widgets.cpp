#include <Veng/UI/Widgets.h>
#include <Veng/UI/Layout.h>
#include <Veng/UI/Theme.h>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
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

    bool Slider(string_view label, i32& v, i32 min, i32 max)
    {
        const string id = AsCStr(label);
        return ImGui::SliderInt(id.c_str(), &v, min, max);
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
        bool InputTextImpl(string_view label, const char* hint, string& v)
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

            const ImGuiInputTextFlags flags =
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize;
            const bool entered =
                hint != nullptr
                    ? ImGui::InputTextWithHint(id.c_str(), hint, s_Scratch.data(), s_Scratch.size(),
                                               flags, resizeCallback, &s_Scratch)
                    : ImGui::InputText(id.c_str(), s_Scratch.data(), s_Scratch.size(), flags,
                                       resizeCallback, &s_Scratch);

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
        return InputTextImpl(label, nullptr, v);
    }

    bool InputTextWithHint(string_view label, string_view hint, string& v)
    {
        const string hintStr = AsCStr(hint);
        return InputTextImpl(label, hintStr.c_str(), v);
    }

    bool Combo(string_view label, i32& index, std::span<const string_view> items)
    {
        const string id = AsCStr(label);

        const string_view preview = (index >= 0 && static_cast<usize>(index) < items.size())
                                        ? items[static_cast<usize>(index)]
                                        : string_view{};
        const string previewStr(preview);

        bool changed = false;
        if (ImGui::BeginCombo(id.c_str(), previewStr.c_str()))
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
        return ImGui::Button(id.c_str());
    }

    bool SmallButton(string_view label)
    {
        const string id = AsCStr(label);
        return ImGui::SmallButton(id.c_str());
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
}
