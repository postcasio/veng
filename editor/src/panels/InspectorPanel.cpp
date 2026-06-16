#include "InspectorPanel.h"

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Vendor/ImGui.h>
#include <VengEditor/EditorRegistry.h>

#include <cstring>

namespace VengEditor
{
    using namespace Veng;

    InspectorPanel::InspectorPanel(AssetManager& assets, EditorRegistry& editors) :
        m_Assets(assets), m_Editors(editors)
    {
    }

    void InspectorPanel::OnImGui()
    {
        if (m_Scene == nullptr || !m_Selected || !m_Scene->IsAlive(*m_Selected))
        {
            ImGui::TextDisabled("Nothing selected");
            return;
        }

        TypeRegistry& types = m_Scene->GetTypeRegistry();
        const Entity entity = *m_Selected;

        m_Scene->ForEachComponent(entity, [&](TypeId id, void* component)
        {
            const TypeInfo& info = types.Info(id);

            // A stable per-type id keeps headers from collapsing into one another
            // when two components share a display name.
            ImGui::PushID(static_cast<int>(id));
            if (ImGui::CollapsingHeader(info.Name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                DrawFields(component, info);
            ImGui::PopID();
        });
    }

    void InspectorPanel::DrawFields(void* base, const TypeInfo& type)
    {
        for (const FieldDescriptor& field : type.Fields)
        {
            if (field.Hidden)
                continue;

            void* fieldPtr = static_cast<u8*>(base) + field.Offset;
            DrawField(fieldPtr, field);
        }
    }

    void InspectorPanel::DrawField(void* fieldPtr, const FieldDescriptor& field)
    {
        // A game-registered custom widget for this field's type overrides the
        // built-in chosen from FieldClass.
        if (const FieldWidgetFn* custom = m_Editors.FieldWidgetFor(field.Type))
        {
            (*custom)(fieldPtr, field);
            return;
        }

        TypeRegistry& types = m_Scene->GetTypeRegistry();

        const string& label = field.DisplayName.empty() ? field.Name : field.DisplayName;
        const char* labelText = label.c_str();

        ImGui::PushID(labelText);

        const f32 step = field.Step ? static_cast<f32>(*field.Step) : 0.01f;
        const f32 minV = field.Min ? static_cast<f32>(*field.Min) : 0.0f;
        const f32 maxV = field.Max ? static_cast<f32>(*field.Max) : 0.0f;
        const bool hasRange = field.Min.has_value() || field.Max.has_value();
        const ImGuiSliderFlags rangeFlag = hasRange ? ImGuiSliderFlags_AlwaysClamp : 0;

        switch (field.Class)
        {
        case FieldClass::Scalar:
        {
            if (field.Type == types.IdOf<f32>())
            {
                ImGui::DragFloat(labelText, static_cast<f32*>(fieldPtr), step, minV, maxV, "%.3f", rangeFlag);
            }
            else if (field.Type == types.IdOf<i32>())
            {
                ImGui::DragInt(labelText, static_cast<i32*>(fieldPtr));
            }
            else if (field.Type == types.IdOf<u32>())
            {
                i32 value = static_cast<i32>(*static_cast<u32*>(fieldPtr));
                if (ImGui::DragInt(labelText, &value, 1.0f, 0))
                    *static_cast<u32*>(fieldPtr) = static_cast<u32>(value < 0 ? 0 : value);
            }
            else if (field.Type == types.IdOf<bool>())
            {
                ImGui::Checkbox(labelText, static_cast<bool*>(fieldPtr));
            }
            else
            {
                ImGui::LabelText(labelText, "(scalar)");
            }
            break;
        }
        case FieldClass::Vector:
        {
            if (field.Type == types.IdOf<vec2>())
                ImGui::DragFloat2(labelText, glm::value_ptr(*static_cast<vec2*>(fieldPtr)), step);
            else if (field.Type == types.IdOf<vec3>())
                ImGui::DragFloat3(labelText, glm::value_ptr(*static_cast<vec3*>(fieldPtr)), step);
            else if (field.Type == types.IdOf<vec4>())
                ImGui::DragFloat4(labelText, glm::value_ptr(*static_cast<vec4*>(fieldPtr)), step);
            break;
        }
        case FieldClass::Quaternion:
        {
            quat& q = *static_cast<quat*>(fieldPtr);
            vec3 euler = glm::degrees(glm::eulerAngles(q));
            const string eulerLabel = label + " (Euler °)";
            if (ImGui::DragFloat3(eulerLabel.c_str(), glm::value_ptr(euler), 0.5f))
                q = quat(glm::radians(euler));
            break;
        }
        case FieldClass::String:
        {
            string& value = *static_cast<string*>(fieldPtr);
            char buffer[256];
            std::strncpy(buffer, value.c_str(), sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';
            if (ImGui::InputText(labelText, buffer, sizeof(buffer),
                                 ImGuiInputTextFlags_EnterReturnsTrue))
            {
                value = buffer;
            }
            else if (ImGui::IsItemDeactivatedAfterEdit())
            {
                value = buffer;
            }
            break;
        }
        case FieldClass::AssetHandle:
        {
            // The AssetId is the leading u64 of any AssetHandle<T> (offset 0 is
            // pinned by AssetHandle's layout guard), so it is readable through the
            // erased field pointer without naming the asset type.
            const u64 id = *static_cast<const u64*>(fieldPtr);
            if (id == 0)
                ImGui::LabelText(labelText, "(none)");
            else
                ImGui::LabelText(labelText, "0x%llX", static_cast<unsigned long long>(id));
            break;
        }
        case FieldClass::Reference:
        {
            const Entity& ref = *static_cast<const Entity*>(fieldPtr);
            if (ref.IsNull())
                ImGui::LabelText(labelText, "(null)");
            else
                ImGui::LabelText(labelText, "Entity %u:%u", ref.Index, ref.Generation);
            break;
        }
        case FieldClass::Matrix:
        {
            const mat4& m = *static_cast<const mat4*>(fieldPtr);
            if (ImGui::TreeNodeEx(labelText, ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                for (int row = 0; row < 4; ++row)
                    ImGui::Text("% .3f  % .3f  % .3f  % .3f",
                                m[0][row], m[1][row], m[2][row], m[3][row]);
                ImGui::TreePop();
            }
            break;
        }
        case FieldClass::Enum:
        {
            // No enum-value table is recorded in the reflection layer; show the
            // backing integer read-only until a value table exists.
            const i32 value = *static_cast<const i32*>(fieldPtr);
            ImGui::LabelText(labelText, "%d", value);
            break;
        }
        case FieldClass::Struct:
        {
            const TypeInfo& nested = types.Info(field.Type);
            if (ImGui::TreeNodeEx(labelText, ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                DrawFields(fieldPtr, nested);
                ImGui::TreePop();
            }
            break;
        }
        }

        if (!field.Tooltip.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", field.Tooltip.c_str());

        ImGui::PopID();
    }
}
