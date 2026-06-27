#include "panels/InspectorPanel.h"

#include "CommandStack.h"
#include "EditorCommand.h"
#include "EditorIcons.h"
#include "FieldWidget.h"

#include <Veng/Asset/AssetManager.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/UI/UI.h>
#include <Veng/Vendor/ImGui.h>
#include <VengEditor/EditorRegistry.h>

#include <algorithm>
#include <cctype>

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
                                   const AssetSourceIndex& sources, PrefabEditContext& ctx,
                                   CommandStack& commands)
        : m_Assets(assets), m_Editors(editors), m_Sources(sources), m_Ctx(ctx), m_Commands(commands)
    {
    }

    void InspectorPanel::OnUI()
    {
        Scene* scene = m_Ctx.Scene;
        const Entity entity = m_Ctx.Active;
        if (scene == nullptr || entity.IsNull() || !scene->IsAlive(entity))
        {
            // A pending edit whose entity vanished can never resolve — drop it.
            m_PendingEdit.reset();
            UI::TextDisabled("Nothing selected");
            return;
        }

        UI::SeparatorText("Components");

        TypeId removeId = InvalidTypeId;
        bool remove = false;
        scene->ForEachComponent(entity, [&](TypeId id, void* component)
                                { DrawComponent(entity, id, component, removeId, remove); });

        // A structural change is illegal mid-ForEachComponent; build the remove command now the
        // walk has returned. Snapshot the component's bytes first so the command can restore it.
        if (remove)
        {
            void* component = scene->TryGetComponent(entity, removeId);
            if (component != nullptr)
            {
                const TypeInfo& info = scene->GetTypeRegistry().Info(removeId);
                vector<u8> snapshot;
                WriteFields(snapshot, component, info, scene->GetTypeRegistry());
                m_Commands.Push(
                    CreateUnique<RemoveComponentCommand>(entity, removeId, std::move(snapshot)));
            }
        }

        // Commit a coalesced field edit once no widget is being actively dragged any longer, so a
        // whole drag is one command. The component still holds the live (post-edit) value, so the
        // "after" bytes are read fresh here.
        if (m_PendingEdit.has_value() && !ImGui::IsAnyItemActive())
        {
            const PendingEdit pending = std::move(*m_PendingEdit);
            m_PendingEdit.reset();
            if (scene->IsAlive(pending.Entity))
            {
                void* component = scene->TryGetComponent(pending.Entity, pending.Type);
                if (component != nullptr)
                {
                    const TypeInfo& info = scene->GetTypeRegistry().Info(pending.Type);
                    vector<u8> after;
                    WriteFields(after, component, info, scene->GetTypeRegistry());
                    if (after != pending.Before)
                    {
                        m_Commands.Push(CreateUnique<EditField>(pending.Entity, pending.Type,
                                                                pending.Before, std::move(after)));
                    }
                }
            }
        }

        UI::Dummy(vec2{0.0f, 4.0f});
        DrawAddComponent(entity);
    }

    void InspectorPanel::DrawComponent(Entity entity, TypeId id, void* component,
                                       TypeId& outRemoveId, bool& outRemove)
    {
        const TypeRegistry& types = m_Ctx.Scene->GetTypeRegistry();
        const TypeInfo& info = types.Info(id);

        // A stable per-type id keeps headers from collapsing into one another when two
        // components share a display name.
        auto scope = UI::PushId(fmt::format("{}", id));

        // The header shows the bare type name with its namespace de-emphasized beside it.
        // ImGui's header takes one flat label, so draw a hidden-label collapsing header
        // (full-width, collapsible) and overlay the styled type label over its text area,
        // past the disclosure arrow and vertically centered in the frame.
        const vec2 headerOrigin = UI::CursorPos();
        auto header = UI::CollapsingHeader("##header", UI::TreeFlags::DefaultOpen);
        const vec2 afterHeader = UI::CursorPos();
        const f32 vpad = (UI::GetFrameHeight() - UI::GetTextLineHeight()) * 0.5f;
        UI::SetCursorPos(vec2{headerOrigin.x + UI::GetFrameHeight(), headerOrigin.y + vpad});
        UI::TypeLabel(info.Name, info.Namespace);
        UI::SetCursorPos(afterHeader);
        const bool open = static_cast<bool>(header);

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
                // Snapshot the live bytes so the reset is undoable, then push the command (it
                // re-runs the destruct + default-construct so Redo matches Apply exactly).
                vector<u8> snapshot;
                WriteFields(snapshot, component, info, types);
                m_Commands.Push(
                    CreateUnique<ResetComponentCommand>(entity, id, std::move(snapshot)));
            }
        }

        if (!open)
        {
            return;
        }

        // Snapshot the component's pre-edit bytes before drawing the widgets, so a "changed" this
        // frame can open a coalescing field edit whose "before" is this snapshot.
        vector<u8> before;
        WriteFields(before, component, info, types);

        const FieldWidgetContext ctx{
            .Assets = m_Assets, .Sources = m_Sources, .Editors = m_Editors};
        bool changed = false;
        if (auto table = UI::PropertyTable("##fields"))
        {
            changed = DrawFields(component, info.Fields, ctx);
        }

        if (changed)
        {
            // An edit to a MeshRenderer's recipe source regenerates no mesh on its own; re-resolve
            // so the derived mesh rebuilds live while dragging (the command re-runs this too).
            m_Ctx.ResolveEntity(entity);

            // Open the coalescing edit on the first changed frame for this component; a continuing
            // drag keeps the original "before". A switch to a different entity/component commits
            // nothing here — OnUI commits the prior pending edit once no item is active.
            if (!m_PendingEdit.has_value() || m_PendingEdit->Entity != entity ||
                m_PendingEdit->Type != id)
            {
                m_PendingEdit =
                    PendingEdit{.Entity = entity, .Type = id, .Before = std::move(before)};
            }
        }
    }

    void InspectorPanel::DrawAddComponent(Entity entity)
    {
        Scene* scene = m_Ctx.Scene;

        if (UI::Button(fmt::format("{} Add Component", Icons::Add)))
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
            // A full-width selectable carries the click; the styled type label (name plus
            // de-emphasized namespace) is overlaid over its row.
            const vec2 rowOrigin = UI::CursorPos();
            const bool picked = UI::Selectable(fmt::format("##add{}", id));
            const vec2 afterRow = UI::CursorPos();
            UI::SetCursorPos(rowOrigin);
            UI::TypeLabel(info.Name, info.Namespace);
            UI::SetCursorPos(afterRow);
            if (picked)
            {
                // The command adds the component and resolves the entity (a newly added
                // MeshRenderer carrying a recipe source builds no mesh until resolved); Revert
                // removes it.
                m_Commands.Push(CreateUnique<AddComponentCommand>(entity, id));
            }
        }

        // Each row ends by restoring the cursor with SetCursorPos; ImGui aborts if a popup
        // closes on a dangling SetCursorPos with no item submitted after it. Anchor it.
        UI::Dummy(vec2{0.0f, 0.0f});
    }
}
