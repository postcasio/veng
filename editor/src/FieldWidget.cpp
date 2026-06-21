#include "FieldWidget.h"

#include "AssetSourceIndex.h"
#include "panels/PrefabEditContext.h"

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>
#include <Veng/UI/UI.h>
#include <VengEditor/EditorRegistry.h>

#include <cstring>

namespace VengEditor
{
    using namespace Veng;

    optional<AssetType> AssetTypeOfHandle(TypeId type)
    {
        if (type == TypeIdOf<AssetHandle<Texture>>())
        {
            return AssetType::Texture;
        }
        if (type == TypeIdOf<AssetHandle<Mesh>>())
        {
            return AssetType::Mesh;
        }
        if (type == TypeIdOf<AssetHandle<Material>>())
        {
            return AssetType::Material;
        }
        return std::nullopt;
    }

    void ApplyAssetPick(void* fieldPtr, AssetId chosen)
    {
        const u64 value = chosen.Value;
        std::memcpy(fieldPtr, &value, sizeof(value));
    }

    namespace
    {
        // Combo over the manifest's ids of the field's asset type; "(none)" clears it.
        void DrawAssetPicker(void* fieldPtr, const FieldDescriptor& field, string_view label,
                             const FieldWidgetContext& ctx)
        {
            u64 currentId = 0;
            std::memcpy(&currentId, fieldPtr, sizeof(currentId));

            const optional<AssetType> assetType = AssetTypeOfHandle(field.Type);
            if (!assetType)
            {
                // No enumeration for this handle type — show the raw id read-only.
                if (currentId == 0)
                {
                    UI::TextDisabled("(none)");
                }
                else
                {
                    UI::TextDisabled(fmt::format("0x{:X}", currentId));
                }
                return;
            }

            // Index 0 is "(none)" (clears the handle); index N picks candidate N-1.
            const vector<AssetId> candidates = ctx.Sources.EntriesOfType(*assetType);

            vector<string> labels;
            labels.reserve(candidates.size() + 1);
            labels.emplace_back("(none)");
            for (const AssetId candidate : candidates)
            {
                labels.push_back(fmt::format("0x{:X}", candidate.Value));
            }

            vector<string_view> items(labels.begin(), labels.end());

            i32 index = 0;
            for (usize i = 0; i < candidates.size(); ++i)
            {
                if (candidates[i].Value == currentId)
                {
                    index = static_cast<i32>(i) + 1;
                    break;
                }
            }

            if (UI::Combo(label, index, items))
            {
                if (index == 0)
                {
                    ApplyAssetPick(fieldPtr, AssetId{});
                }
                else
                {
                    ApplyAssetPick(fieldPtr, candidates[static_cast<usize>(index) - 1]);
                }
            }
        }

        // Reads an Entity drop on the previous widget; returns the dropped entity or nullopt.
        optional<Entity> AcceptEntityDrop()
        {
            auto target = UI::DragDropTarget();
            if (!target)
            {
                return std::nullopt;
            }
            const void* payload = UI::AcceptDragDropPayload(PrefabEditContext::EntityPayload);
            if (payload == nullptr)
            {
                return std::nullopt;
            }
            Entity dropped{};
            std::memcpy(&dropped, payload, sizeof(dropped));
            return dropped;
        }

        // A drop target plus a clear button for an intra-scene Entity reference field.
        void DrawReference(void* fieldPtr, const FieldDescriptor& field, string_view label)
        {
            Entity& ref = *static_cast<Entity*>(fieldPtr);

            if (field.ReadOnly)
            {
                if (ref.IsNull())
                {
                    UI::TextDisabled("(null)");
                }
                else
                {
                    UI::TextDisabled(fmt::format("Entity {}:{}", ref.Index, ref.Generation));
                }
                return;
            }

            const string text = ref.IsNull()
                                    ? string{"(null)"}
                                    : fmt::format("Entity {}:{}", ref.Index, ref.Generation);
            // A label-less button is the drop surface; the id keeps it unique per field.
            (void)UI::Button(fmt::format("{}##{}", text, label));
            if (const optional<Entity> dropped = AcceptEntityDrop())
            {
                ref = *dropped;
            }

            if (!ref.IsNull())
            {
                UI::SameLine();
                if (UI::SmallButton(fmt::format("x##clear{}", label)))
                {
                    ref = Entity::Null;
                }
            }
        }

        // Editable integer view over an enum's backing bytes (width from its TypeInfo).
        void DrawEnum(void* fieldPtr, const FieldDescriptor& field, string_view label,
                      const FieldWidgetContext& ctx)
        {
            const usize width = ctx.Assets.GetTypeRegistry().Info(field.Type).Size;

            i64 value = 0;
            std::memcpy(&value, fieldPtr, width < sizeof(value) ? width : sizeof(value));

            i32 edited = static_cast<i32>(value);
            if (UI::Drag(label, edited, UI::DragOptions{.Speed = 0.1f, .Min = 0.0f}))
            {
                const i64 clamped = edited < 0 ? 0 : edited;
                std::memcpy(fieldPtr, &clamped, width < sizeof(clamped) ? width : sizeof(clamped));
            }
        }
    }

    // Recurses each non-hidden field of a struct/active-alternative as an indented row;
    // shared by the Struct and Variant cases.
    static void DrawStructFields(void* base, const TypeInfo& info, const FieldWidgetContext& ctx)
    {
        UI::Indent();
        for (const FieldDescriptor& nestedField : info.Fields)
        {
            if (nestedField.Hidden)
            {
                continue;
            }
            void* nestedPtr = static_cast<u8*>(base) + nestedField.Offset;
            DrawFieldWidget(nestedPtr, nestedField, ctx);
        }
        UI::Unindent();
    }

    namespace
    {
        // Combo over the variant's alternatives (plus "(none)"); switching activates a
        // default-constructed alternative or clears to empty, then the active member's fields
        // recurse as indented rows beneath the combo.
        void DrawVariant(void* fieldPtr, const FieldDescriptor& field, string_view label,
                         const FieldWidgetContext& ctx)
        {
            const TypeRegistry& registry = ctx.Assets.GetTypeRegistry();
            const TypeInfo& info = registry.Info(field.Type);

            // Index 0 is "(none)" (clears the variant); index N activates alternative N-1.
            const vector<TypeId>& alternatives = info.VariantAlternatives;

            vector<string> labels;
            labels.reserve(alternatives.size() + 1);
            labels.emplace_back("(none)");
            for (const TypeId altId : alternatives)
            {
                labels.push_back(registry.Info(altId).Name);
            }

            vector<string_view> items(labels.begin(), labels.end());

            const TypeId activeType = info.VariantActiveType(fieldPtr);
            i32 index = 0;
            for (usize i = 0; i < alternatives.size(); ++i)
            {
                if (alternatives[i] == activeType)
                {
                    index = static_cast<i32>(i) + 1;
                    break;
                }
            }

            if (UI::Combo(label, index, items))
            {
                if (index == 0)
                {
                    info.VariantClear(fieldPtr);
                }
                else
                {
                    info.VariantSetActive(fieldPtr, alternatives[static_cast<usize>(index) - 1]);
                }
            }

            void* activePtr = info.VariantActivePtr(fieldPtr);
            if (activePtr != nullptr)
            {
                const TypeInfo& activeInfo = registry.Info(info.VariantActiveType(fieldPtr));
                DrawStructFields(activePtr, activeInfo, ctx);
            }
        }
    }

    void DrawFieldWidget(void* fieldPtr, const FieldDescriptor& field,
                         const FieldWidgetContext& ctx)
    {
        if (field.Hidden)
        {
            return;
        }

        const string& displayName = field.DisplayName.empty() ? field.Name : field.DisplayName;
        const string valueLabel = "##" + field.Name;

        // A custom widget owns its whole row, including the property label.
        if (const FieldWidgetFn* custom = ctx.Editors.FieldWidgetFor(field.Type))
        {
            UI::PropertyLabel(displayName);
            (*custom)(fieldPtr, field);
            if (!field.Tooltip.empty())
            {
                UI::Tooltip(field.Tooltip);
            }
            return;
        }

        // A nested struct flattens into further indented rows in the same table — never a
        // nested BeginTable inside a cell.
        if (field.Class == FieldClass::Struct)
        {
            UI::PropertyLabel(displayName);
            // Scope nested rows under the field name so a nested member sharing a name with
            // an outer field keeps a distinct widget id.
            auto structScope = UI::PushId(valueLabel);
            const TypeInfo& nested = ctx.Assets.GetTypeRegistry().Info(field.Type);
            DrawStructFields(fieldPtr, nested, ctx);
            return;
        }

        // A variant draws an alternative-picker combo on its own row, then flattens the active
        // alternative's fields into indented rows the same way a nested struct does.
        if (field.Class == FieldClass::Variant)
        {
            UI::PropertyLabel(displayName);
            auto variantScope = UI::PushId(valueLabel);
            DrawVariant(fieldPtr, field, valueLabel, ctx);
            if (!field.Tooltip.empty())
            {
                UI::Tooltip(field.Tooltip);
            }
            return;
        }

        UI::PropertyLabel(displayName);

        auto id = UI::PushId(valueLabel);

        // The Reference field carries its own read-only handling (drop target vs. label);
        // every other class shares one Disabled scope keyed on ReadOnly.
        if (field.Class == FieldClass::Reference)
        {
            DrawReference(fieldPtr, field, valueLabel);
            if (!field.Tooltip.empty())
            {
                UI::Tooltip(field.Tooltip);
            }
            return;
        }

        auto disabled = UI::Disabled(field.ReadOnly);

        // Drag speed and clamp range come from the field's optional editor metadata;
        // absent metadata leaves the DragOptions defaults (0.01f speed, unclamped).
        UI::DragOptions drag;
        if (field.Step)
        {
            drag.Speed = static_cast<f32>(*field.Step);
        }
        if (field.Min)
        {
            drag.Min = static_cast<f32>(*field.Min);
        }
        if (field.Max)
        {
            drag.Max = static_cast<f32>(*field.Max);
        }

        switch (field.Class)
        {
        case FieldClass::Scalar:
        {
            if (field.Type == TypeIdOf<f32>())
            {
                (void)UI::Drag(valueLabel, *static_cast<f32*>(fieldPtr), drag);
            }
            else if (field.Type == TypeIdOf<i32>())
            {
                (void)UI::Drag(valueLabel, *static_cast<i32*>(fieldPtr));
            }
            else if (field.Type == TypeIdOf<u32>())
            {
                // u32 has no Drag overload; edit through a signed view clamped to 0+.
                i32 value = static_cast<i32>(*static_cast<u32*>(fieldPtr));
                if (UI::Drag(valueLabel, value, UI::DragOptions{.Min = 0.0f}))
                {
                    *static_cast<u32*>(fieldPtr) = static_cast<u32>(value < 0 ? 0 : value);
                }
            }
            else if (field.Type == TypeIdOf<bool>())
            {
                (void)UI::Checkbox(valueLabel, *static_cast<bool*>(fieldPtr));
            }
            else
            {
                UI::TextDisabled("(scalar)");
            }
            break;
        }
        case FieldClass::Vector:
        {
            if (field.Type == TypeIdOf<vec2>())
            {
                (void)UI::Drag(valueLabel, *static_cast<vec2*>(fieldPtr), drag);
            }
            else if (field.Type == TypeIdOf<vec3>())
            {
                (void)UI::Drag(valueLabel, *static_cast<vec3*>(fieldPtr), drag);
            }
            else if (field.Type == TypeIdOf<vec4>())
            {
                (void)UI::Drag(valueLabel, *static_cast<vec4*>(fieldPtr), drag);
            }
            break;
        }
        case FieldClass::Quaternion:
        {
            quat& q = *static_cast<quat*>(fieldPtr);
            vec3 euler = glm::degrees(glm::eulerAngles(q));
            if (UI::Drag(valueLabel, euler, UI::DragOptions{.Speed = 0.5f}))
            {
                q = quat(glm::radians(euler));
            }
            break;
        }
        case FieldClass::String:
        {
            string& value = *static_cast<string*>(fieldPtr);
            (void)UI::InputText(valueLabel, value);
            break;
        }
        case FieldClass::AssetHandle:
        {
            DrawAssetPicker(fieldPtr, field, valueLabel, ctx);
            break;
        }
        case FieldClass::Matrix:
        {
            const mat4& m = *static_cast<const mat4*>(fieldPtr);
            for (int row = 0; row < 4; ++row)
            {
                UI::Text(fmt::format("{: .3f}  {: .3f}  {: .3f}  {: .3f}", m[0][row], m[1][row],
                                     m[2][row], m[3][row]));
            }
            break;
        }
        case FieldClass::Enum:
        {
            DrawEnum(fieldPtr, field, valueLabel, ctx);
            break;
        }
        case FieldClass::Reference:
        case FieldClass::Struct:
        case FieldClass::Variant:
        {
            // Handled above the switch; unreachable here.
            break;
        }
        }

        if (!field.Tooltip.empty())
        {
            UI::Tooltip(field.Tooltip);
        }
    }
}
