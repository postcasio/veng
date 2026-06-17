#include "FieldWidget.h"

#include "AssetSourceIndex.h"

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Vendor/ImGui.h>
#include <VengEditor/EditorRegistry.h>

#include <cstring>

namespace VengEditor
{
    using namespace Veng;

    optional<AssetType> AssetTypeOfHandle(TypeId type)
    {
        if (type == TypeIdOf<AssetHandle<Texture>>()) return AssetType::Texture;
        if (type == TypeIdOf<AssetHandle<Mesh>>()) return AssetType::Mesh;
        if (type == TypeIdOf<AssetHandle<Material>>()) return AssetType::Material;
        return std::nullopt;
    }

    void ApplyAssetPick(void* fieldPtr, AssetId chosen)
    {
        const u64 value = chosen.Value;
        std::memcpy(fieldPtr, &value, sizeof(value));
    }

    namespace
    {
        // The AssetHandle picker: a combo over the manifest's ids of the field's
        // asset type, writing the chosen id back through the leading u64 of the
        // handle (offset 0 is pinned by AssetHandle's layout guard, so the id is
        // writable without naming the asset's concrete type). "(none)" clears it.
        void DrawAssetPicker(void* fieldPtr, const FieldDescriptor& field, const char* labelText,
                             const FieldWidgetContext& ctx)
        {
            u64 currentId = 0;
            std::memcpy(&currentId, fieldPtr, sizeof(currentId));

            const optional<AssetType> assetType = AssetTypeOfHandle(field.Type);
            if (!assetType)
            {
                // No enumeration for this handle type — fall back to the id label.
                if (currentId == 0)
                    ImGui::LabelText(labelText, "(none)");
                else
                    ImGui::LabelText(labelText, "0x%llX", static_cast<unsigned long long>(currentId));
                return;
            }

            char preview[32];
            if (currentId == 0)
                std::snprintf(preview, sizeof(preview), "(none)");
            else
                std::snprintf(preview, sizeof(preview), "0x%llX",
                              static_cast<unsigned long long>(currentId));

            if (ImGui::BeginCombo(labelText, preview))
            {
                if (ImGui::Selectable("(none)", currentId == 0))
                    ApplyAssetPick(fieldPtr, AssetId{});

                for (const AssetId candidate : ctx.Sources.EntriesOfType(*assetType))
                {
                    char label[32];
                    std::snprintf(label, sizeof(label), "0x%llX",
                                  static_cast<unsigned long long>(candidate.Value));
                    if (ImGui::Selectable(label, candidate.Value == currentId))
                        ApplyAssetPick(fieldPtr, candidate);
                }

                ImGui::EndCombo();
            }
        }
    }

    void DrawFieldWidget(void* fieldPtr, const FieldDescriptor& field, const FieldWidgetContext& ctx)
    {
        if (field.Hidden)
            return;

        // A game-registered custom widget for this field's type overrides the
        // built-in chosen from FieldClass.
        if (const FieldWidgetFn* custom = ctx.Editors.FieldWidgetFor(field.Type))
        {
            (*custom)(fieldPtr, field);
            return;
        }

        const string& label = field.DisplayName.empty() ? field.Name : field.DisplayName;
        const char* labelText = label.c_str();

        ImGui::PushID(labelText);

        const f32 step = field.Step ? static_cast<f32>(*field.Step) : 0.01f;
        const f32 minV = field.Min ? static_cast<f32>(*field.Min) : -FLT_MAX;
        const f32 maxV = field.Max ? static_cast<f32>(*field.Max) : FLT_MAX;
        const bool hasRange = field.Min.has_value() || field.Max.has_value();
        const ImGuiSliderFlags rangeFlag = hasRange ? ImGuiSliderFlags_AlwaysClamp : 0;

        switch (field.Class)
        {
        case FieldClass::Scalar:
        {
            if (field.Type == TypeIdOf<f32>())
            {
                ImGui::DragFloat(labelText, static_cast<f32*>(fieldPtr), step, minV, maxV, "%.3f", rangeFlag);
            }
            else if (field.Type == TypeIdOf<i32>())
            {
                ImGui::DragInt(labelText, static_cast<i32*>(fieldPtr));
            }
            else if (field.Type == TypeIdOf<u32>())
            {
                i32 value = static_cast<i32>(*static_cast<u32*>(fieldPtr));
                if (ImGui::DragInt(labelText, &value, 1.0f, 0))
                    *static_cast<u32*>(fieldPtr) = static_cast<u32>(value < 0 ? 0 : value);
            }
            else if (field.Type == TypeIdOf<bool>())
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
            if (field.Type == TypeIdOf<vec2>())
                ImGui::DragFloat2(labelText, glm::value_ptr(*static_cast<vec2*>(fieldPtr)), step);
            else if (field.Type == TypeIdOf<vec3>())
                ImGui::DragFloat3(labelText, glm::value_ptr(*static_cast<vec3*>(fieldPtr)), step);
            else if (field.Type == TypeIdOf<vec4>())
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
            DrawAssetPicker(fieldPtr, field, labelText, ctx);
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
            const TypeInfo& nested = ctx.Assets.GetTypeRegistry().Info(field.Type);
            if (ImGui::TreeNodeEx(labelText, ImGuiTreeNodeFlags_SpanAvailWidth))
            {
                for (const FieldDescriptor& nestedField : nested.Fields)
                {
                    if (nestedField.Hidden)
                        continue;
                    void* nestedPtr = static_cast<u8*>(fieldPtr) + nestedField.Offset;
                    DrawFieldWidget(nestedPtr, nestedField, ctx);
                }
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
