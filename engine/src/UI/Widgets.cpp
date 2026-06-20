#include <Veng/UI/Widgets.h>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

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
    }

    bool Drag(string_view label, f32& v, DragOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::DragFloat(id.c_str(), &v, options.Speed,
                                options.Min.value_or(0.0f), options.Max.value_or(0.0f),
                                FloatFormat(options), DragClampFlags(options));
    }

    bool Drag(string_view label, vec2& v, DragOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::DragFloat2(id.c_str(), glm::value_ptr(v), options.Speed,
                                 options.Min.value_or(0.0f), options.Max.value_or(0.0f),
                                 FloatFormat(options), DragClampFlags(options));
    }

    bool Drag(string_view label, vec3& v, DragOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::DragFloat3(id.c_str(), glm::value_ptr(v), options.Speed,
                                 options.Min.value_or(0.0f), options.Max.value_or(0.0f),
                                 FloatFormat(options), DragClampFlags(options));
    }

    bool Drag(string_view label, vec4& v, DragOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::DragFloat4(id.c_str(), glm::value_ptr(v), options.Speed,
                                 options.Min.value_or(0.0f), options.Max.value_or(0.0f),
                                 FloatFormat(options), DragClampFlags(options));
    }

    bool Drag(string_view label, i32& v, DragOptions options)
    {
        const string id = AsCStr(label);
        const i32 min = options.Min ? static_cast<i32>(*options.Min) : 0;
        const i32 max = options.Max ? static_cast<i32>(*options.Max) : 0;
        return ImGui::DragInt(id.c_str(), &v, options.Speed, min, max,
                              IntFormat(options), DragClampFlags(options));
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

    bool InputText(string_view label, string& v)
    {
        const string id = AsCStr(label);

        // Write-back happens only on Enter or deactivate-after-edit. A single
        // static scratch is safe: ImGui activates at most one input item at a time,
        // so the scratch is owned by whichever item's id matches s_ActiveId.
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

        const bool entered = ImGui::InputText(
            id.c_str(), s_Scratch.data(), s_Scratch.size(),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackResize,
            resizeCallback, &s_Scratch);

        // ImGui::IsItemActive is valid only after the widget is submitted, so the
        // scratch's owning id is updated here, after InputText.
        if (ImGui::IsItemActive())
            s_ActiveId = itemId;
        else if (s_ActiveId == itemId)
            s_ActiveId = 0;

        if (entered || ImGui::IsItemDeactivatedAfterEdit())
        {
            v = s_Scratch.data();
            return true;
        }
        return false;
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
                    ImGui::SetItemDefaultFocus();
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

    bool Selectable(string_view label, bool selected)
    {
        const string id = AsCStr(label);
        return ImGui::Selectable(id.c_str(), selected);
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
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopStyleColor();
    }

    void Label(string_view label, string_view value)
    {
        // LabelText is printf-only; pass the string_view value via "%.*s".
        const string id = AsCStr(label);
        ImGui::LabelText(id.c_str(), "%.*s", static_cast<int>(value.size()), value.data());
    }

    void Image(const Ref<ImGuiTexture>& tex, vec2 size)
    {
        ImGui::Image(static_cast<ImTextureID>(tex->GetTextureId()), size);
    }
}
