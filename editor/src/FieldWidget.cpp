#include "FieldWidget.h"

#include "AssetDragPayload.h"
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
        // Returns true when the pick changed the handle.
        bool DrawAssetPicker(void* fieldPtr, const FieldDescriptor& field, string_view label,
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
                return false;
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

            bool changed = false;
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
                changed = true;
            }

            // Accept an asset dragged from the browser when its type matches this field.
            if (auto target = UI::DragDropTarget())
            {
                if (const void* payload = UI::AcceptDragDropPayload(AssetPayload))
                {
                    AssetDragPayload dropped{};
                    std::memcpy(&dropped, payload, sizeof(dropped));
                    if (dropped.Type == *assetType)
                    {
                        ApplyAssetPick(fieldPtr, dropped.Id);
                        changed = true;
                    }
                }
            }

            return changed;
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
        // Returns true when the reference changed (a drop or a clear).
        bool DrawReference(void* fieldPtr, const FieldDescriptor& field, string_view label)
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
                return false;
            }

            bool changed = false;
            const string text = ref.IsNull()
                                    ? string{"(null)"}
                                    : fmt::format("Entity {}:{}", ref.Index, ref.Generation);
            // A label-less button is the drop surface; the id keeps it unique per field.
            (void)UI::Button(fmt::format("{}##{}", text, label));
            if (const optional<Entity> dropped = AcceptEntityDrop())
            {
                ref = *dropped;
                changed = true;
            }

            if (!ref.IsNull())
            {
                UI::SameLine();
                if (UI::SmallButton(fmt::format("x##clear{}", label)))
                {
                    ref = Entity::Null;
                    changed = true;
                }
            }
            return changed;
        }

        // Editable integer view over an enum's backing bytes (width from its TypeInfo).
        // Returns true when the value changed.
        bool DrawEnum(void* fieldPtr, const FieldDescriptor& field, string_view label,
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
                return true;
            }
            return false;
        }
    }

    // Recurses each non-hidden field of a struct/active-alternative as an indented row;
    // shared by the Struct, Variant, and Array-element cases. Returns true when any nested
    // field changed.
    static bool DrawStructFields(void* base, const TypeInfo& info, const FieldWidgetContext& ctx)
    {
        bool changed = false;
        UI::Indent();
        for (const FieldDescriptor& nestedField : info.Fields)
        {
            if (nestedField.Hidden)
            {
                continue;
            }
            void* nestedPtr = static_cast<u8*>(base) + nestedField.Offset;
            changed |= DrawFieldWidget(nestedPtr, nestedField, ctx);
        }
        UI::Unindent();
        return changed;
    }

    namespace
    {
        // Returns the alternative's AssetHandle<Material> field, or nullptr if it has none.
        // Every primitive shape carries one, so it is the field preserved across a type switch.
        const FieldDescriptor* MaterialField(const TypeInfo& info)
        {
            for (const FieldDescriptor& field : info.Fields)
            {
                if (field.Type == TypeIdOf<AssetHandle<Material>>())
                {
                    return &field;
                }
            }
            return nullptr;
        }

        // Combo over the variant's alternatives (plus "(none)"); switching activates a
        // default-constructed alternative or clears to empty, then the active member's fields
        // recurse as indented rows beneath the combo. Returns true when the active alternative
        // changed or any of its fields was edited.
        bool DrawVariant(void* fieldPtr, const FieldDescriptor& field, string_view label,
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
                const TypeInfo& altInfo = registry.Info(altId);
                labels.push_back(UI::FormatTypeLabel(altInfo.Name, altInfo.Namespace));
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

            bool changed = false;
            if (UI::Combo(label, index, items))
            {
                // Carry the material across the switch: a default-constructed alternative has a
                // null material, and the renderer skips a submesh that has none — so without this
                // a type change would rebuild a mesh that renders nothing. The handle is copied
                // out before SetActive destructs the outgoing alternative; shared scalar
                // parameters intentionally reset to the new shape's defaults.
                AssetHandle<Material> carried;
                if (const void* oldActive = info.VariantActivePtrConst(fieldPtr))
                {
                    const TypeInfo& oldInfo = registry.Info(info.VariantActiveType(fieldPtr));
                    if (const FieldDescriptor* matField = MaterialField(oldInfo))
                    {
                        carried = *reinterpret_cast<const AssetHandle<Material>*>(
                            static_cast<const u8*>(oldActive) + matField->Offset);
                    }
                }

                if (index == 0)
                {
                    info.VariantClear(fieldPtr);
                }
                else
                {
                    info.VariantSetActive(fieldPtr, alternatives[static_cast<usize>(index) - 1]);
                }
                changed = true;

                if (void* newActive = info.VariantActivePtr(fieldPtr))
                {
                    const TypeInfo& newInfo = registry.Info(info.VariantActiveType(fieldPtr));
                    if (const FieldDescriptor* matField = MaterialField(newInfo))
                    {
                        *reinterpret_cast<AssetHandle<Material>*>(
                            static_cast<u8*>(newActive) + matField->Offset) = std::move(carried);
                    }
                }
            }

            void* activePtr = info.VariantActivePtr(fieldPtr);
            if (activePtr != nullptr)
            {
                const TypeInfo& activeInfo = registry.Info(info.VariantActiveType(fieldPtr));
                changed |= DrawStructFields(activePtr, activeInfo, ctx);
            }
            return changed;
        }

        // An add/remove list over a FieldClass::Array field. The label row carries an Add
        // button; each element flattens its fields as indented rows beneath a per-element row
        // bearing a Remove button. Reorder is not offered — the configuration list is order-
        // independent. Returns true when an element was added, removed, or edited.
        bool DrawArray(void* fieldPtr, const FieldDescriptor& field, string_view label,
                       const FieldWidgetContext& ctx)
        {
            const TypeRegistry& registry = ctx.Assets.GetTypeRegistry();
            const TypeInfo& elementInfo = registry.Info(field.ElementType);

            bool changed = false;

            // The value cell of the field's own row holds the Add button.
            if (UI::SmallButton(fmt::format("Add##add{}", label)))
            {
                const usize count = field.ArraySize(fieldPtr);
                field.ArrayResize(fieldPtr, count + 1);
                changed = true;
            }

            const usize count = field.ArraySize(fieldPtr);
            UI::Indent();
            // A remove defers to after the element loop so the array is not resized mid-walk.
            optional<usize> removeAt;
            for (usize i = 0; i < count; ++i)
            {
                void* element = field.ArrayElement(fieldPtr, i);
                auto elementScope = UI::PushId(fmt::format("{}elem{}", label, i));

                UI::PropertyLabel(fmt::format("[{}]", i));
                if (UI::SmallButton("Remove##remove"))
                {
                    removeAt = i;
                }

                changed |= DrawStructFields(element, elementInfo, ctx);
            }
            UI::Unindent();

            if (removeAt)
            {
                // Shift each later element down one by destruct + move-construct, then drop the
                // tail: ArrayResize destructs the now-moved-from last slot. Uses only the
                // Destruct/MoveConstruct ops the reflection layer exposes (no element copy).
                for (usize i = *removeAt; i + 1 < count; ++i)
                {
                    void* dst = field.ArrayElement(fieldPtr, i);
                    void* src = field.ArrayElement(fieldPtr, i + 1);
                    elementInfo.Destruct(dst);
                    elementInfo.MoveConstruct(dst, src);
                }
                field.ArrayResize(fieldPtr, count - 1);
                changed = true;
            }

            return changed;
        }
    }

    bool DrawFieldWidget(void* fieldPtr, const FieldDescriptor& field,
                         const FieldWidgetContext& ctx)
    {
        if (field.Hidden)
        {
            return false;
        }

        const string& displayName = field.DisplayName.empty() ? field.Name : field.DisplayName;
        const string valueLabel = "##" + field.Name;

        // A custom widget owns its whole row, including the property label. Its void
        // signature carries no change signal, so a custom-widget edit does not re-resolve.
        if (const FieldWidgetFn* custom = ctx.Editors.FieldWidgetFor(field.Type))
        {
            UI::PropertyLabel(displayName);
            (*custom)(fieldPtr, field);
            if (!field.Tooltip.empty())
            {
                UI::Tooltip(field.Tooltip);
            }
            return false;
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
            return DrawStructFields(fieldPtr, nested, ctx);
        }

        // A variant draws an alternative-picker combo on its own row, then flattens the active
        // alternative's fields into indented rows the same way a nested struct does.
        if (field.Class == FieldClass::Variant)
        {
            UI::PropertyLabel(displayName);
            auto variantScope = UI::PushId(valueLabel);
            const bool changed = DrawVariant(fieldPtr, field, valueLabel, ctx);
            if (!field.Tooltip.empty())
            {
                UI::Tooltip(field.Tooltip);
            }
            return changed;
        }

        // An array draws an Add button on its own row, then flattens each element's fields into
        // indented rows beneath a per-element row carrying a Remove button.
        if (field.Class == FieldClass::Array)
        {
            UI::PropertyLabel(displayName);
            auto arrayScope = UI::PushId(valueLabel);
            const bool changed = DrawArray(fieldPtr, field, valueLabel, ctx);
            if (!field.Tooltip.empty())
            {
                UI::Tooltip(field.Tooltip);
            }
            return changed;
        }

        UI::PropertyLabel(displayName);

        auto id = UI::PushId(valueLabel);

        // The Reference field carries its own read-only handling (drop target vs. label);
        // every other class shares one Disabled scope keyed on ReadOnly.
        if (field.Class == FieldClass::Reference)
        {
            const bool changed = DrawReference(fieldPtr, field, valueLabel);
            if (!field.Tooltip.empty())
            {
                UI::Tooltip(field.Tooltip);
            }
            return changed;
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

        bool changed = false;
        switch (field.Class)
        {
        case FieldClass::Scalar:
        {
            if (field.Type == TypeIdOf<f32>())
            {
                changed = UI::Drag(valueLabel, *static_cast<f32*>(fieldPtr), drag);
            }
            else if (field.Type == TypeIdOf<i32>())
            {
                changed = UI::Drag(valueLabel, *static_cast<i32*>(fieldPtr));
            }
            else if (field.Type == TypeIdOf<u32>())
            {
                // u32 has no Drag overload; edit through a signed view clamped to 0+.
                i32 value = static_cast<i32>(*static_cast<u32*>(fieldPtr));
                if (UI::Drag(valueLabel, value, UI::DragOptions{.Min = 0.0f}))
                {
                    *static_cast<u32*>(fieldPtr) = static_cast<u32>(value < 0 ? 0 : value);
                    changed = true;
                }
            }
            else if (field.Type == TypeIdOf<bool>())
            {
                changed = UI::Checkbox(valueLabel, *static_cast<bool*>(fieldPtr));
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
                changed = UI::Drag(valueLabel, *static_cast<vec2*>(fieldPtr), drag);
            }
            else if (field.Type == TypeIdOf<vec3>())
            {
                changed = UI::Drag(valueLabel, *static_cast<vec3*>(fieldPtr), drag);
            }
            else if (field.Type == TypeIdOf<vec4>())
            {
                changed = UI::Drag(valueLabel, *static_cast<vec4*>(fieldPtr), drag);
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
                changed = true;
            }
            break;
        }
        case FieldClass::String:
        {
            string& value = *static_cast<string*>(fieldPtr);
            changed = UI::InputText(valueLabel, value);
            break;
        }
        case FieldClass::AssetHandle:
        {
            changed = DrawAssetPicker(fieldPtr, field, valueLabel, ctx);
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
            changed = DrawEnum(fieldPtr, field, valueLabel, ctx);
            break;
        }
        case FieldClass::Reference:
        case FieldClass::Struct:
        case FieldClass::Variant:
        case FieldClass::Array:
        {
            // Handled above the switch; unreachable here.
            break;
        }
        }

        if (!field.Tooltip.empty())
        {
            UI::Tooltip(field.Tooltip);
        }
        return changed;
    }
}
