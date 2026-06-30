#include "FieldWidget.h"

#include "AssetChip.h"
#include "AssetSourceIndex.h"
#include "EditorIcons.h"
#include "FieldGate.h"
#include "FieldWidgetDispatch.h"
#include "panels/PrefabEditContext.h"

#include <Veng/Asset/Animation.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Environment.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Log.h>
#include <Veng/Reflection/FieldDisplay.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Entity.h>
#include <Veng/UI/UI.h>
#include <VengEditor/EditorRegistry.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>

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
        if (type == TypeIdOf<AssetHandle<MaterialInstance>>())
        {
            return AssetType::MaterialInstance;
        }
        if (type == TypeIdOf<AssetHandle<Prefab>>())
        {
            return AssetType::Prefab;
        }
        if (type == TypeIdOf<AssetHandle<Animation>>())
        {
            return AssetType::Animation;
        }
        if (type == TypeIdOf<AssetHandle<EnvironmentMap>>())
        {
            return AssetType::Environment;
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
        // An asset chip standing in for the handle: a drop target that doubles as a
        // click-to-search selector for the field's asset type. Returns true when the pick
        // (a drop or a popup selection) changed the handle.
        bool DrawAssetPicker(void* fieldPtr, const FieldDescriptor& field, string_view label,
                             const FieldWidgetContext& ctx)
        {
            u64 currentId = 0;
            std::memcpy(&currentId, fieldPtr, sizeof(currentId));

            // A handle type the picker can't enumerate (no AssetType mapping) draws as a static
            // chip; an enumerable one is an interactive drop target / selector.
            const optional<AssetType> assetType = AssetTypeOfHandle(field.Type);

            const AssetChipInfo chip{
                .Id = AssetId{currentId},
                .Type = assetType.value_or(AssetType::Raw),
                .IdScope = label,
                .DropTarget = assetType.has_value() && !field.ReadOnly,
            };

            const optional<AssetId> picked = DrawAssetChip(chip, ctx.Sources);
            if (picked)
            {
                ApplyAssetPick(fieldPtr, *picked);
                return true;
            }
            return false;
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
                if (UI::SmallButton(fmt::format("{}##clear{}", Icons::Remove, label)))
                {
                    ref = Entity::Null;
                    changed = true;
                }
                UI::Tooltip("Clear the reference");
            }
            return changed;
        }

        // A named combo over the enum's {name, value} table when its TypeInfo carries one,
        // or an editable integer view over the backing bytes (width from its TypeInfo) when it
        // does not. A backing value matching no enumerator shows a synthesized "(unknown N)"
        // label so it is never silently wrong; picking a named entry repairs it. Returns true
        // when the value changed.
        bool DrawEnum(void* fieldPtr, const FieldDescriptor& field, string_view label,
                      const FieldWidgetContext& ctx)
        {
            const TypeInfo& info = ctx.Assets.GetTypeRegistry().Info(field.Type);
            const usize width = info.Size;

            // Widen the backing bytes into an i64 (low Size bytes, little-endian).
            i64 value = 0;
            std::memcpy(&value, fieldPtr, width < sizeof(value) ? width : sizeof(value));

            if (info.Enumerators.empty())
            {
                i32 edited = static_cast<i32>(value);
                if (UI::Drag(label, edited, UI::DragOptions{.Speed = 0.1f, .Min = 0.0f}))
                {
                    const i64 clamped = edited < 0 ? 0 : edited;
                    std::memcpy(fieldPtr, &clamped,
                                width < sizeof(clamped) ? width : sizeof(clamped));
                    return true;
                }
                return false;
            }

            // The combo lists each enumerator's name; a backing value matching none appends a
            // synthesized "(unknown N)" entry, selected so the drift is visible and editable.
            vector<string> labels;
            labels.reserve(info.Enumerators.size() + 1);
            for (const EnumEntry& entry : info.Enumerators)
            {
                labels.push_back(entry.Name);
            }

            i32 index = -1;
            for (usize i = 0; i < info.Enumerators.size(); ++i)
            {
                if (info.Enumerators[i].Value == value)
                {
                    index = static_cast<i32>(i);
                    break;
                }
            }
            if (index < 0)
            {
                labels.push_back(fmt::format("(unknown {})", value));
                index = static_cast<i32>(labels.size()) - 1;
            }

            const vector<string_view> items(labels.begin(), labels.end());
            if (UI::Combo(label, index, items))
            {
                // The synthesized entry is not a real enumerator, so a pick lands only on a
                // named one; writing back narrows the chosen value into the low Size bytes.
                if (index >= 0 && static_cast<usize>(index) < info.Enumerators.size())
                {
                    const i64 chosen = info.Enumerators[static_cast<usize>(index)].Value;
                    std::memcpy(fieldPtr, &chosen, width < sizeof(chosen) ? width : sizeof(chosen));
                    return true;
                }
            }
            return false;
        }
    }

    // Recurses each non-hidden field of a struct/active-alternative as an indented row;
    // shared by the Struct, Variant, and Array-element cases. Routes through the shared
    // DrawFields walk so nested fields get the same Category grouping the top-level walk does.
    // Returns true when any nested field changed.
    static bool DrawStructFields(void* base, const TypeInfo& info, const FieldWidgetContext& ctx)
    {
        UI::Indent();
        const bool changed = DrawFields(base, info.Fields, ctx);
        UI::Unindent();
        return changed;
    }

    namespace
    {
        // Draws one field within a DrawFields walk, after the Hidden/Category filter has placed
        // it. A failing VisibleIf skips the row; a failing EnabledIf disables the row, composing
        // with ReadOnly so a field is editable only when both allow it. `ownerBase` is the base
        // of the struct this walk iterates, against which both predicates evaluate.
        bool DrawGatedField(void* base, const FieldDescriptor& field, const FieldWidgetContext& ctx)
        {
            const void* ownerBase = ctx.OwnerBase;
            if (!IsFieldVisible(field, ownerBase))
            {
                return false;
            }
            void* fieldPtr = static_cast<u8*>(base) + field.Offset;
            auto disabled = UI::Disabled(!IsFieldEnabled(field, ownerBase));
            return DrawFieldWidget(fieldPtr, field, ctx);
        }
    }

    bool DrawFields(void* base, std::span<const FieldDescriptor> fields,
                    const FieldWidgetContext& outerCtx)
    {
        // Re-seed the owner base to the struct this walk iterates, so a field's predicate reads
        // its immediate owner — a nested struct's recursion (through DrawStructFields →
        // DrawFields) seeds the nested base, an array element seeds the element base.
        FieldWidgetContext ctx = outerCtx;
        ctx.OwnerBase = base;

        bool changed = false;

        // Draw the un-categorized fields first, in declared order; gather the categories in
        // first-seen order so the grouped sections below keep a predictable layout.
        vector<string_view> categories;
        for (const FieldDescriptor& field : fields)
        {
            if (field.Hidden)
            {
                continue;
            }
            if (field.Category.empty())
            {
                changed |= DrawGatedField(base, field, ctx);
                continue;
            }
            if (std::ranges::find(categories, field.Category) == categories.end())
            {
                categories.emplace_back(field.Category);
            }
        }

        // Each category renders under a full-width collapsing header, its fields in declared
        // order. The header is a no-op extra row when no field carries a Category.
        for (const string_view category : categories)
        {
            if (!UI::PropertyHeader(category))
            {
                continue;
            }
            for (const FieldDescriptor& field : fields)
            {
                if (field.Hidden || field.Category != category)
                {
                    continue;
                }
                changed |= DrawGatedField(base, field, ctx);
            }
        }

        return changed;
    }

    namespace
    {
        // Draws the editable value widget(s) for a leaf/reference field into the current
        // property-table value column — the caller has already drawn the label and pushed the
        // field id. Shared by DrawFieldWidget (every non-composite field) and DrawArray (a
        // leaf element). Returns true when the value changed.
        bool DrawValueWidget(void* fieldPtr, const FieldDescriptor& field, string_view valueLabel,
                             const FieldWidgetContext& ctx);

        // Returns the alternative's AssetHandle<MaterialInstance> field, or nullptr if it has none.
        // Every primitive shape carries one, so it is the field preserved across a type switch.
        const FieldDescriptor* MaterialField(const TypeInfo& info)
        {
            for (const FieldDescriptor& field : info.Fields)
            {
                if (field.Type == TypeIdOf<AssetHandle<MaterialInstance>>())
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
                AssetHandle<MaterialInstance> carried;
                if (const void* oldActive = info.VariantActivePtrConst(fieldPtr))
                {
                    const TypeInfo& oldInfo = registry.Info(info.VariantActiveType(fieldPtr));
                    if (const FieldDescriptor* matField = MaterialField(oldInfo))
                    {
                        carried = *reinterpret_cast<const AssetHandle<MaterialInstance>*>(
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
                        *reinterpret_cast<AssetHandle<MaterialInstance>*>(
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

        // An add/remove list over a FieldClass::Array field. The label row carries an Add button.
        // A leaf element (scalar/vector/string/enum/asset-handle/reference) draws as a single
        // value row with a trailing Remove button. A struct/variant element draws its fields
        // either flat (a per-element row bearing a Remove button) or, when `collapsible`, under a
        // foldable header whose row carries the Remove button. Reorder is not offered — the
        // configuration list is order-independent.
        //
        // The element fold state is ImGui-owned, keyed on the positional per-element id
        // ({label}elem{i}); the elements have no stable identity to key on, so removing element
        // [i] slides every later element's fold state down one. This is harmless precisely
        // because the list is order-independent.
        //
        // Returns true when an element was added, removed, or edited.
        bool DrawArray(void* fieldPtr, const FieldDescriptor& field, string_view label,
                       const FieldWidgetContext& ctx, bool collapsible, bool defaultOpen)
        {
            const TypeRegistry& registry = ctx.Assets.GetTypeRegistry();
            const TypeInfo& elementInfo = registry.Info(field.ElementType);

            bool changed = false;

            // The value cell of the field's own row holds the Add button.
            if (UI::SmallButton(fmt::format("{}##add{}", Icons::Add, label)))
            {
                const usize count = field.ArraySize(fieldPtr);
                field.ArrayResize(fieldPtr, count + 1);
                changed = true;
            }
            UI::Tooltip("Append an element");

            const usize count = field.ArraySize(fieldPtr);
            UI::Indent();
            // A leaf element type has no nested fields to fold; collapsibility applies only to a
            // struct/variant element, which recurses its fields. Loop-invariant in the element
            // type, so it is resolved once.
            const bool elementIsLeaf =
                elementInfo.Class != FieldClass::Struct && elementInfo.Class != FieldClass::Variant;
            // A remove defers to after the element loop so the array is not resized mid-walk.
            optional<usize> removeAt;
            for (usize i = 0; i < count; ++i)
            {
                void* element = field.ArrayElement(fieldPtr, i);
                auto elementScope = UI::PushId(fmt::format("{}elem{}", label, i));

                if (elementIsLeaf)
                {
                    // One row: the element's value widget fills the column, the Remove button
                    // trails it. The synthetic descriptor carries the element type plus the array
                    // field's read-only state and presentation hints (Min/Max/Step/Widget).
                    UI::PropertyLabel(fmt::format("[{}]", i));
                    UI::SetNextItemWidth(-(UI::GetFrameHeight() + 4.0f));
                    FieldDescriptor elementField;
                    elementField.Type = field.ElementType;
                    elementField.Class = elementInfo.Class;
                    elementField.ReadOnly = field.ReadOnly;
                    elementField.Display = field.Display;
                    changed |= DrawValueWidget(element, elementField, "##elemval", ctx);
                    UI::SameLine();
                    if (UI::SmallButton(fmt::format("{}##remove", Icons::Remove)))
                    {
                        removeAt = i;
                    }
                    continue;
                }

                if (collapsible)
                {
                    // The header (spanning both columns) carries the element's fold; the Remove
                    // button sits in the value column of the same row, so a collapsed element's
                    // removal stays reachable without expanding it.
                    const bool open =
                        UI::PropertyHeader(fmt::format("[{}]##elemhdr", i), defaultOpen);
                    UI::TableSetColumnIndex(1);
                    if (UI::SmallButton(fmt::format("{}##remove", Icons::Remove)))
                    {
                        removeAt = i;
                    }
                    if (open)
                    {
                        changed |= DrawStructFields(element, elementInfo, ctx);
                    }
                    continue;
                }

                UI::PropertyLabel(fmt::format("[{}]", i));
                if (UI::SmallButton(fmt::format("{}##remove", Icons::Remove)))
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

    namespace
    {
        // The display name of a WidgetKind, for the one-time incompatible-hint warning.
        const char* WidgetKindName(WidgetKind kind)
        {
            switch (kind)
            {
            case WidgetKind::Auto:
                return "Auto";
            case WidgetKind::Drag:
                return "Drag";
            case WidgetKind::Slider:
                return "Slider";
            case WidgetKind::Color:
                return "Color";
            case WidgetKind::Multiline:
                return "Multiline";
            }
            return "?";
        }

        // Warns once per field descriptor when its resolved widget hint was degraded. The
        // seen-set is keyed on the FieldDescriptor pointer — stable for the registry's
        // lifetime — and read on the single render thread, so it needs no synchronization.
        // It is cleared when the owning registry changes, so a freed descriptor's pointer
        // cannot be reused as a stale key.
        void WarnDegradedHintOnce(const TypeRegistry& registry, const FieldDescriptor& field,
                                  WidgetKind requested, WidgetKind effective)
        {
            static const TypeRegistry* s_LastRegistry = nullptr;
            static std::unordered_set<const FieldDescriptor*> s_Warned;

            if (s_LastRegistry != &registry)
            {
                s_Warned.clear();
                s_LastRegistry = &registry;
            }

            if (!s_Warned.insert(&field).second)
            {
                return;
            }

            Log::Warn("Field '{}' requests widget {} incompatible with its type; drawing {} "
                      "instead.",
                      field.Name, WidgetKindName(requested), WidgetKindName(effective));
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
        // nested BeginTable inside a cell. When the field resolves Collapsible, the struct
        // instead folds under a full-width header whose body is the same recursion.
        if (field.Class == FieldClass::Struct)
        {
            // Scope nested rows under the field name so a nested member sharing a name with
            // an outer field keeps a distinct widget id.
            auto structScope = UI::PushId(valueLabel);
            const TypeRegistry& registry = ctx.Assets.GetTypeRegistry();
            const TypeInfo& nested = registry.Info(field.Type);
            const FieldDisplay display = ResolveFieldDisplay(field, registry);
            if (display.Collapsible.value_or(false))
            {
                if (!UI::PropertyHeader(displayName, display.DefaultOpen.value_or(true)))
                {
                    return false;
                }
                return DrawStructFields(fieldPtr, nested, ctx);
            }
            UI::PropertyLabel(displayName);
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

        // An array draws an Add button on its own row, then each element's fields beneath it.
        // Each element is a foldable header by default so a long list stays scannable; a field
        // resolving Collapsible=false flattens the elements into indented rows instead.
        if (field.Class == FieldClass::Array)
        {
            UI::PropertyLabel(displayName);
            auto arrayScope = UI::PushId(valueLabel);
            const FieldDisplay display = ResolveFieldDisplay(field, ctx.Assets.GetTypeRegistry());
            const bool changed =
                DrawArray(fieldPtr, field, valueLabel, ctx, display.Collapsible.value_or(true),
                          display.DefaultOpen.value_or(true));
            if (!field.Tooltip.empty())
            {
                UI::Tooltip(field.Tooltip);
            }
            return changed;
        }

        UI::PropertyLabel(displayName);

        auto id = UI::PushId(valueLabel);
        return DrawValueWidget(fieldPtr, field, valueLabel, ctx);
    }

    namespace
    {
        bool DrawValueWidget(void* fieldPtr, const FieldDescriptor& field, string_view valueLabel,
                             const FieldWidgetContext& ctx)
        {
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

            // Drag speed and clamp range come from the field's resolved presentation
            // (field override over type default); absent metadata leaves the DragOptions
            // defaults (0.01f speed, unclamped).
            const TypeRegistry& registry = ctx.Assets.GetTypeRegistry();
            const FieldDisplay display = ResolveFieldDisplay(field, registry);
            UI::DragOptions drag;
            if (display.Step)
            {
                drag.Speed = static_cast<f32>(*display.Step);
            }
            if (display.Min)
            {
                drag.Min = static_cast<f32>(*display.Min);
            }
            if (display.Max)
            {
                drag.Max = static_cast<f32>(*display.Max);
            }

            // The widget the field actually draws — its resolved hint, degraded if it is
            // incompatible with the field's class/type (a slider needs a range, a color needs a
            // vec3/vec4, multiline needs a string). A degrade warns once per descriptor.
            const bool hasRange = display.Min.has_value() && display.Max.has_value();
            const WidgetKind effective =
                EffectiveWidget(display.Widget, field.Class, field.Type, hasRange);
            if (effective != display.Widget && display.Widget != WidgetKind::Auto)
            {
                WarnDegradedHintOnce(registry, field, display.Widget, effective);
            }

            // A Slider reads the resolved Min/Max; SliderOptions bounds are f32, so the
            // optional<f64> cascade values narrow at the call site.
            UI::SliderOptions slider;
            if (display.Min)
            {
                slider.Min = static_cast<f32>(*display.Min);
            }
            if (display.Max)
            {
                slider.Max = static_cast<f32>(*display.Max);
            }

            bool changed = false;
            switch (field.Class)
            {
            case FieldClass::Scalar:
            {
                if (field.Type == TypeIdOf<f32>())
                {
                    if (effective == WidgetKind::Slider)
                    {
                        changed = UI::Slider(valueLabel, *static_cast<f32*>(fieldPtr), slider);
                    }
                    else
                    {
                        changed = UI::Drag(valueLabel, *static_cast<f32*>(fieldPtr), drag);
                    }
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
                    if (effective == WidgetKind::Slider)
                    {
                        changed = UI::Slider(valueLabel, *static_cast<vec2*>(fieldPtr), slider);
                    }
                    else
                    {
                        changed = UI::Drag(valueLabel, *static_cast<vec2*>(fieldPtr), drag);
                    }
                }
                else if (field.Type == TypeIdOf<vec3>())
                {
                    if (effective == WidgetKind::Color)
                    {
                        changed = UI::ColorEdit3(valueLabel, *static_cast<vec3*>(fieldPtr));
                    }
                    else if (effective == WidgetKind::Slider)
                    {
                        changed = UI::Slider(valueLabel, *static_cast<vec3*>(fieldPtr), slider);
                    }
                    else
                    {
                        changed = UI::Drag(valueLabel, *static_cast<vec3*>(fieldPtr), drag);
                    }
                }
                else if (field.Type == TypeIdOf<vec4>())
                {
                    if (effective == WidgetKind::Color)
                    {
                        changed = UI::ColorEdit4(valueLabel, *static_cast<vec4*>(fieldPtr));
                    }
                    else if (effective == WidgetKind::Slider)
                    {
                        changed = UI::Slider(valueLabel, *static_cast<vec4*>(fieldPtr), slider);
                    }
                    else
                    {
                        changed = UI::Drag(valueLabel, *static_cast<vec4*>(fieldPtr), drag);
                    }
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
                if (effective == WidgetKind::Multiline)
                {
                    changed = UI::InputTextMultiline(valueLabel, value);
                }
                else
                {
                    changed = UI::InputText(valueLabel, value);
                }
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
}
