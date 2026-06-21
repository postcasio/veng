#include "panels/InspectorPanel.h"

#include "FieldWidget.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/UI/UI.h>
#include <VengEditor/EditorRegistry.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // Case-insensitive substring test, for the Add Component search filter.
        bool ContainsNoCase(string_view haystack, string_view needle)
        {
            if (needle.empty())
            {
                return true;
            }
            const auto lower = [](char c)
            { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            const auto it = std::ranges::search(haystack, needle, {}, lower, lower).begin();
            return it != haystack.end();
        }
    }

    InspectorPanel::InspectorPanel(AssetManager& assets, EditorRegistry& editors,
                                   const AssetSourceIndex& sources, PrefabEditContext& ctx)
        : m_Assets(assets), m_Editors(editors), m_Sources(sources), m_Ctx(ctx)
    {
        // A named combo for LightType reads better than the generic editable-integer enum
        // path; registered once here rather than special-cased inside the enum widget.
        m_Editors.RegisterFieldWidget(
            TypeIdOf<LightType>(),
            [](void* fieldPtr, const FieldDescriptor&)
            {
                static constexpr std::array<string_view, 3> Names{"Directional", "Point", "Spot"};
                u32 raw = 0;
                std::memcpy(&raw, fieldPtr, sizeof(raw));
                i32 index = raw < Names.size() ? static_cast<i32>(raw) : 0;
                if (UI::Combo("##lighttype", index, Names))
                {
                    raw = static_cast<u32>(index);
                    std::memcpy(fieldPtr, &raw, sizeof(raw));
                }
            });
    }

    void InspectorPanel::OnImGui()
    {
        Scene* scene = m_Ctx.Scene;
        const Entity entity = m_Ctx.Active;
        if (scene == nullptr || entity.IsNull() || !scene->IsAlive(entity))
        {
            UI::TextDisabled("Nothing selected");
            return;
        }

        DrawHeader(entity);
        UI::SeparatorText("Components");

        TypeId removeId = InvalidTypeId;
        bool remove = false;
        scene->ForEachComponent(entity, [&](TypeId id, void* component)
                                { DrawComponent(entity, id, component, removeId, remove); });

        // A structural change is illegal mid-ForEachComponent; apply the queued remove now.
        if (remove)
        {
            scene->RemoveComponent(entity, removeId);
        }

        UI::Dummy(vec2{0.0f, 4.0f});
        DrawAddComponent(entity);
    }

    void InspectorPanel::DrawHeader(Entity entity)
    {
        Scene* scene = m_Ctx.Scene;

        // Reload the scratch from the live name whenever the active entity changes.
        if (m_NameFor != entity)
        {
            const Name* name = scene->TryGet<Name>(entity);
            m_NameScratch = name != nullptr ? name->Value : string{};
            m_NameFor = entity;
        }

        if (UI::InputTextWithHint("##name", "Entity name", m_NameScratch))
        {
            if (Name* name = scene->TryGet<Name>(entity))
            {
                name->Value = m_NameScratch;
            }
            else
            {
                scene->Add<Name>(entity).Value = m_NameScratch;
            }
        }

        UI::TextDisabled(fmt::format("Entity {}:{}", entity.Index, entity.Generation));
    }

    void InspectorPanel::DrawComponent(Entity entity, TypeId id, void* component,
                                       TypeId& outRemoveId, bool& outRemove)
    {
        const TypeRegistry& types = m_Ctx.Scene->GetTypeRegistry();
        const TypeInfo& info = types.Info(id);

        // A stable per-type id keeps headers from collapsing into one another when two
        // components share a display name.
        auto scope = UI::PushId(fmt::format("{}", id));

        const bool open =
            static_cast<bool>(UI::CollapsingHeader(info.Name, UI::TreeFlags::DefaultOpen));

        // Hierarchy topology is owned by the hierarchy panel, so it offers no removal.
        const bool removable = id != TypeIdOf<Hierarchy>();
        if (auto menu = UI::PopupContextItem("##compmenu"))
        {
            if (removable && UI::MenuItem("Remove Component"))
            {
                outRemoveId = id;
                outRemove = true;
            }
            if (UI::MenuItem("Reset to Default"))
            {
                info.Destruct(component);
                info.DefaultConstruct(component);
            }
        }

        if (!open)
        {
            return;
        }

        const FieldWidgetContext ctx{
            .Assets = m_Assets, .Sources = m_Sources, .Editors = m_Editors};
        if (auto table = UI::PropertyTable("##fields"))
        {
            for (const FieldDescriptor& field : info.Fields)
            {
                if (field.Hidden)
                {
                    continue;
                }
                void* fieldPtr = static_cast<u8*>(component) + field.Offset;
                DrawFieldWidget(fieldPtr, field, ctx);
            }
        }
    }

    void InspectorPanel::DrawAddComponent(Entity entity)
    {
        Scene* scene = m_Ctx.Scene;

        if (UI::Button("Add Component"))
        {
            m_AddSearch.clear();
            UI::OpenPopup("##addcomp");
        }

        auto popup = UI::Popup("##addcomp");
        if (!popup)
        {
            return;
        }

        (void)UI::InputTextWithHint("##addsearch", "Search…", m_AddSearch);

        // The components already on the entity are excluded from the candidate list; there
        // is no type-erased Has, so gather their ids in one enumeration.
        vector<TypeId> present;
        scene->ForEachComponent(entity, [&](TypeId id, void*) { present.push_back(id); });

        const TypeRegistry& types = scene->GetTypeRegistry();
        for (const auto& [id, info] : types.All())
        {
            // Only component-like structs are addable; Hierarchy is hierarchy-panel-owned.
            if (info.Class != FieldClass::Struct || id == TypeIdOf<Hierarchy>())
            {
                continue;
            }
            if (std::ranges::find(present, id) != present.end())
            {
                continue;
            }
            if (!ContainsNoCase(info.Name, m_AddSearch))
            {
                continue;
            }
            if (UI::Selectable(info.Name))
            {
                scene->AddComponent(entity, id);
            }
        }
    }
}
