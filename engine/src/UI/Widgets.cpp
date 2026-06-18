#include <Veng/UI/Widgets.h>

#include <imgui.h>
#include <glm/gtc/type_ptr.hpp>

namespace Veng::UI
{
    namespace
    {
        // ImGui's const char* widget overloads take a single null-terminated
        // string used for both display and ID, with no data+size form (only the
        // *Unformatted text helpers take an end pointer). A Veng::UI label is a
        // string_view, so it is materialized into a temporary null-terminated
        // string at the call boundary.
        string AsCStr(string_view s)
        {
            return string(s);
        }

        // The default DragOptions::Format is a float spec; the integer overload
        // substitutes "%d" unless the caller overrode it.
        const char* IntFormat(const DragOptions& options)
        {
            return options.Format == DragOptions{}.Format ? "%d" : options.Format;
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
                                options.Format, DragClampFlags(options));
    }

    bool Drag(string_view label, vec2& v, DragOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::DragFloat2(id.c_str(), glm::value_ptr(v), options.Speed,
                                 options.Min.value_or(0.0f), options.Max.value_or(0.0f),
                                 options.Format, DragClampFlags(options));
    }

    bool Drag(string_view label, vec3& v, DragOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::DragFloat3(id.c_str(), glm::value_ptr(v), options.Speed,
                                 options.Min.value_or(0.0f), options.Max.value_or(0.0f),
                                 options.Format, DragClampFlags(options));
    }

    bool Drag(string_view label, vec4& v, DragOptions options)
    {
        const string id = AsCStr(label);
        return ImGui::DragFloat4(id.c_str(), glm::value_ptr(v), options.Speed,
                                 options.Min.value_or(0.0f), options.Max.value_or(0.0f),
                                 options.Format, DragClampFlags(options));
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

        // v is written back only on Enter or deactivate-after-edit, never per
        // keystroke. While the item is not the active one, the scratch tracks v so
        // the field shows the current value; once focused, ImGui owns the scratch
        // and v is left untouched until the commit point. ImGui activates at most
        // one input item at a time, so a single file-static scratch (plus the id of
        // whichever item currently owns it) suffices, and the CallbackResize
        // handler grows it to fit (no fixed truncation).
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
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(color.r, color.g, color.b, color.a));
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
        ImGui::PopStyleColor();
    }

    void Label(string_view label, string_view value)
    {
        // ImGui::LabelText is a printf interface; the value is passed as
        // preformatted text through the "%.*s" + length form.
        const string id = AsCStr(label);
        ImGui::LabelText(id.c_str(), "%.*s", static_cast<int>(value.size()), value.data());
    }

    void Image(const Ref<ImGuiTexture>& tex, vec2 size)
    {
        ImGui::Image(static_cast<ImTextureID>(tex->GetTextureId()),
                     ImVec2(size.x, size.y));
    }
}
