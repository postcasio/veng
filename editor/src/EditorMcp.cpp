#include "EditorMcp.h"

#include "AssetEditorPanel.h"
#include "CommandStack.h"
#include "EditorCommand.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneClone.h>

#include <VengEditor/EditorPanel.h>

#include <cstring>

// The read/write reflection walks the inspector consumes through DrawFieldWidget: the same
// FieldsToJson / JsonToFields the world tools use, so a new reflected field appears over MCP
// with zero MCP change. An internal veng::mcp source header, reached via the editor's private
// include of mcp/src.
#include "ReflectToJson.h"

#include <nlohmann/json.hpp>

namespace VengEditor
{
    using namespace Veng;
    using Veng::Mcp::McpMutation;
    using Veng::Mcp::McpMutationKind;
    using Json = nlohmann::json;

    namespace
    {
        /// @brief Resolves a panel by title among the host's open panels, or null.
        EditorPanel* FindPanel(const EditorMcpHost& host, const string& title)
        {
            // The returned panel is mutated by the caller (GetInspectables / OnInspectableChanged
            // are non-const), so the loop pointee stays mutable.
            for (EditorPanel* panel : host.Panels()) // NOLINT(misc-const-correctness)
            {
                if (panel != nullptr && panel->GetTitle() == title)
                {
                    return panel;
                }
            }
            return nullptr;
        }

        /// @brief Finds an inspectable by name in a panel's set, or returns a false optional.
        optional<Inspectable> FindInspectable(EditorPanel& panel, const string& name)
        {
            for (Inspectable& inspectable : panel.GetInspectables())
            {
                if (inspectable.Name == name)
                {
                    return inspectable;
                }
            }
            return std::nullopt;
        }

        /// @brief Serializes one inspectable's fields to a JSON object under { name, type, fields }.
        Json InspectableToJson(const EditorMcpHost& host, const Inspectable& inspectable)
        {
            const TypeInfo& info = host.Types.Info(inspectable.Type);
            return Json{{"name", inspectable.Name},
                        {"type", info.QualifiedName},
                        {"fields", Mcp::FieldsToJson(inspectable.Data, info, host.Types)}};
        }

        /// @brief A short "kind" tag for a panel, from whether it is a save-bearing asset editor.
        string PanelKind(EditorPanel& panel)
        {
            return dynamic_cast<AssetEditorPanel*>(&panel) != nullptr ? "assetEditor" : "panel";
        }

        /// @brief Adds a component seeded with agent-supplied values as one undoable step.
        ///
        /// A routed add-with-values is one logical agent action, so it is one command: Apply adds
        /// the component and restores the seeded bytes; Revert removes it. A single editor.undo of a
        /// routed add therefore removes the whole thing (component and its values), unlike the
        /// inspector's separate Add-then-Edit steps.
        class AddSeededComponentCommand final : public EditorCommand
        {
        public:
            /// @brief Constructs the add-with-values command from the seeded component's bytes.
            /// @param entity  The component's owning entity.
            /// @param typeId  The component's TypeId.
            /// @param bytes   WriteFields bytes of the component after the values were applied.
            AddSeededComponentCommand(Entity entity, TypeId typeId, vector<u8> bytes)
                : m_Entity(entity), m_TypeId(typeId), m_Bytes(std::move(bytes))
            {
            }

            void Apply(PrefabEditContext& ctx) override
            {
                Scene* scene = ctx.Scene;
                if (scene == nullptr || !scene->IsAlive(m_Entity))
                {
                    return;
                }
                const TypeRegistry& types = scene->GetTypeRegistry();
                void* component = scene->AddComponent(m_Entity, m_TypeId);
                const TypeInfo& info = types.Info(m_TypeId);
                const VoidResult read = ReadFields(m_Bytes, component, info, types);
                VE_ASSERT(read.has_value(), "AddSeededComponentCommand: ReadFields failed for '{}'",
                          info.Name);
                // Re-resolve AssetHandle / Reference fields through the loader path, as the field
                // and remove commands do, so a seeded handle rehydrates to the live cache entry
                // (identity on Reference fields). A device-free host with no AssetManager skips it —
                // its components carry no handle fields.
                if (ctx.Assets != nullptr)
                {
                    const EntityRemap identity = [](Entity reference) { return reference; };
                    AssetManager& assets = *ctx.Assets;
                    const AssetHandleFixup rehydrate = [&assets](void* fieldPtr)
                    {
                        AssetId id{};
                        std::memcpy(&id, fieldPtr, sizeof(id));
                        if (id.IsValid())
                        {
                            const Ref<Detail::AssetCacheEntry> entry = assets.CachedEntry(id);
                            if (entry != nullptr)
                            {
                                Detail::RehydrateHandleField(fieldPtr, id, entry);
                            }
                        }
                    };
                    RemapComponentReferences(component, info, types, identity, rehydrate);
                }
                ctx.ResolveEntity(m_Entity);
            }

            void Revert(PrefabEditContext& ctx) override
            {
                Scene* scene = ctx.Scene;
                if (scene != nullptr && scene->IsAlive(m_Entity))
                {
                    scene->RemoveComponent(m_Entity, m_TypeId);
                }
            }

            [[nodiscard]] string_view GetTitle() const override { return "Add Component"; }

        private:
            Entity m_Entity;
            TypeId m_TypeId;
            vector<u8> m_Bytes;
        };
    }

    void RegisterEditorReflectionTools(Mcp::McpServer& server, const EditorMcpHost& host)
    {
        // editor.list_panels — the open panels, each with its inspectable names: the map of what
        // is editable right now.
        {
            Mcp::McpTool tool;
            tool.Name = "editor.list_panels";
            tool.Description =
                "Lists the editor's open panels: title, kind, focused, and the names of their "
                "inspectables (the reflected objects editor.inspect / editor.set_field address). "
                "No arguments.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                const AssetEditorPanel* focused =
                    host.FocusedDocument ? host.FocusedDocument() : nullptr;
                Json panels = Json::array();
                for (EditorPanel* panel : host.Panels())
                {
                    if (panel == nullptr)
                    {
                        continue;
                    }
                    Json names = Json::array();
                    for (const Inspectable& inspectable : panel->GetInspectables())
                    {
                        names.push_back(inspectable.Name);
                    }
                    panels.push_back(Json{{"title", string{panel->GetTitle()}},
                                          {"kind", PanelKind(*panel)},
                                          {"focused", panel == focused},
                                          {"inspectables", std::move(names)}});
                }
                return Json{{"panels", std::move(panels)}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // editor.inspect — a named panel's inspectable(s) serialized through the shared walk.
        {
            Mcp::McpTool tool;
            tool.Name = "editor.inspect";
            tool.Description =
                "Reads a panel's reflected inspectable(s). Argument: "
                "{ panel: <title>, inspectable?: <name> }. Omitting 'inspectable' returns all of "
                "the panel's inspectables. The fields walk the same reflection the inspector "
                "draws.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["panel"],"properties":{"panel":{"type":"string"},)"
                R"("inspectable":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                if (!args.is_object() || !args.contains("panel") || !args["panel"].is_string())
                {
                    return std::unexpected(string("expected { panel: <title> }"));
                }
                EditorPanel* panel = FindPanel(host, args["panel"].get<string>());
                if (panel == nullptr)
                {
                    return std::unexpected(
                        fmt::format("no open panel titled '{}'", args["panel"].get<string>()));
                }

                if (args.contains("inspectable") && args["inspectable"].is_string())
                {
                    const string name = args["inspectable"].get<string>();
                    const optional<Inspectable> inspectable = FindInspectable(*panel, name);
                    if (!inspectable)
                    {
                        return std::unexpected(fmt::format("panel '{}' has no inspectable '{}'",
                                                           args["panel"].get<string>(), name));
                    }
                    return InspectableToJson(host, *inspectable).dump();
                }

                Json all = Json::array();
                for (const Inspectable& inspectable : panel->GetInspectables())
                {
                    all.push_back(InspectableToJson(host, inspectable));
                }
                return Json{{"panel", args["panel"].get<string>()},
                            {"inspectables", std::move(all)}}
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // editor.set_field — a partial write into a named inspectable, then the panel's own reaction.
        {
            Mcp::McpTool tool;
            tool.Name = "editor.set_field";
            tool.Description =
                "Applies a partial field update to a panel's inspectable, then runs the panel's "
                "reaction (recook / mark dirty / live preview). Argument: "
                "{ panel: <title>, inspectable: <name>, values: { <field>: … } }. A field added to "
                "the reflected type is settable here with no MCP change.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["panel","inspectable","values"],"properties":)"
                R"({"panel":{"type":"string"},"inspectable":{"type":"string"},)"
                R"("values":{"type":"object"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                if (!args.is_object() || !args.contains("panel") || !args["panel"].is_string() ||
                    !args.contains("inspectable") || !args["inspectable"].is_string() ||
                    !args.contains("values") || !args["values"].is_object())
                {
                    return std::unexpected(
                        string("expected { panel: <title>, inspectable: <name>, values: {…} }"));
                }
                EditorPanel* panel = FindPanel(host, args["panel"].get<string>());
                if (panel == nullptr)
                {
                    return std::unexpected(
                        fmt::format("no open panel titled '{}'", args["panel"].get<string>()));
                }
                const string name = args["inspectable"].get<string>();
                const optional<Inspectable> inspectable = FindInspectable(*panel, name);
                if (!inspectable)
                {
                    return std::unexpected(fmt::format("panel '{}' has no inspectable '{}'",
                                                       args["panel"].get<string>(), name));
                }

                const TypeInfo& info = host.Types.Info(inspectable->Type);
                const VoidResult applied =
                    Mcp::JsonToFields(args["values"], inspectable->Data, info, host.Types);
                if (!applied)
                {
                    return std::unexpected(applied.error());
                }
                panel->OnInspectableChanged(name);
                return Json{{"panel", args["panel"].get<string>()},
                            {"inspectable", name},
                            {"updated", info.QualifiedName}}
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // editor.save / editor.undo / editor.redo — the focused document's lifecycle virtuals.
        {
            Mcp::McpTool tool;
            tool.Name = "editor.save";
            tool.Description = "Saves the focused asset editor document (its File→Save action). No "
                               "arguments. Errors when no document is focused or it has no save.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                AssetEditorPanel* doc = host.FocusedDocument ? host.FocusedDocument() : nullptr;
                if (doc == nullptr)
                {
                    return std::unexpected(string("no focused document to save"));
                }
                const VoidResult saved = doc->Save();
                if (!saved)
                {
                    return std::unexpected(saved.error());
                }
                return Json{{"saved", string{doc->GetTitle()}}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        {
            Mcp::McpTool tool;
            tool.Name = "editor.undo";
            tool.Description = "Undoes the last edit on the focused document's command stack. No "
                               "arguments. Errors when no document is focused or it has no undo "
                               "stack; a no-op when the stack is empty.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                AssetEditorPanel* doc = host.FocusedDocument ? host.FocusedDocument() : nullptr;
                if (doc == nullptr)
                {
                    return std::unexpected(string("no focused document to undo"));
                }
                CommandStack* stack = doc->GetCommandStack();
                if (stack == nullptr)
                {
                    return std::unexpected(string("the focused document has no undo stack"));
                }
                const bool did = stack->CanUndo();
                stack->Undo();
                return Json{{"undone", did}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        {
            Mcp::McpTool tool;
            tool.Name = "editor.redo";
            tool.Description =
                "Redoes the last undone edit on the focused document's command stack. "
                "No arguments. Errors when no document is focused or it has no undo "
                "stack; a no-op when nothing was undone.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                AssetEditorPanel* doc = host.FocusedDocument ? host.FocusedDocument() : nullptr;
                if (doc == nullptr)
                {
                    return std::unexpected(string("no focused document to redo"));
                }
                CommandStack* stack = doc->GetCommandStack();
                if (stack == nullptr)
                {
                    return std::unexpected(string("the focused document has no undo stack"));
                }
                const bool did = stack->CanRedo();
                stack->Redo();
                return Json{{"redone", did}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }

    bool ApplyEditorMutation(const EditorMcpHost& host, const McpMutation& mutation)
    {
        AssetEditorPanel* doc = host.FocusedDocument ? host.FocusedDocument() : nullptr;
        Scene* scene = host.DocumentScene ? host.DocumentScene() : nullptr;
        if (doc == nullptr || scene == nullptr)
        {
            return false;
        }
        CommandStack* stack = doc->GetCommandStack();
        if (stack == nullptr)
        {
            return false;
        }
        if (mutation.Target.IsNull() || !scene->IsAlive(mutation.Target))
        {
            return false;
        }

        const TypeRegistry& types = scene->GetTypeRegistry();

        // Parses a mutation's Values JSON string into a scratch component's bytes, both taken as
        // WriteFields snapshots so the resulting EditField command undoes to the pre-write bytes.
        const auto snapshot = [&types](void* component, const TypeInfo& info)
        {
            vector<u8> bytes;
            WriteFields(bytes, component, info, types);
            return bytes;
        };

        switch (mutation.Kind)
        {
        case McpMutationKind::AddComponent:
        {
            const TypeInfo& info = types.Info(mutation.Component);
            const Json values = mutation.Values.empty()
                                    ? Json::object()
                                    : Json::parse(mutation.Values, nullptr, false);
            // Build the seeded bytes on a temporary default component (added, filled, removed) so
            // the pushed command is one undoable unit carrying add + values together — a single
            // editor.undo removes the whole thing, and redo re-adds it with its values intact.
            void* scratch = scene->AddComponent(mutation.Target, mutation.Component);
            const VoidResult applied = Mcp::JsonToFields(values, scratch, info, types);
            if (!applied)
            {
                scene->RemoveComponent(mutation.Target, mutation.Component);
                return false;
            }
            vector<u8> bytes = snapshot(scratch, info);
            scene->RemoveComponent(mutation.Target, mutation.Component);
            stack->Push(CreateUnique<AddSeededComponentCommand>(mutation.Target, mutation.Component,
                                                                std::move(bytes)));
            return true;
        }
        case McpMutationKind::RemoveComponent:
        {
            void* component = scene->TryGetComponent(mutation.Target, mutation.Component);
            if (component == nullptr)
            {
                return false;
            }
            const TypeInfo& info = types.Info(mutation.Component);
            stack->Push(CreateUnique<RemoveComponentCommand>(mutation.Target, mutation.Component,
                                                             snapshot(component, info)));
            return true;
        }
        case McpMutationKind::SetField:
        {
            void* component = scene->TryGetComponent(mutation.Target, mutation.Component);
            if (component == nullptr)
            {
                return false;
            }
            const TypeInfo& info = types.Info(mutation.Component);
            vector<u8> before = snapshot(component, info);
            const Json values = Json::parse(mutation.Values, nullptr, false);
            if (!Mcp::JsonToFields(values, component, info, types))
            {
                return false;
            }
            vector<u8> after = snapshot(component, info);
            stack->Push(CreateUnique<EditField>(mutation.Target, mutation.Component,
                                                std::move(before), std::move(after)));
            return true;
        }
        case McpMutationKind::SpawnEntity:
        case McpMutationKind::DestroyEntity:
        case McpMutationKind::LoadPrefab:
            // The command-routed set is the per-component edits the inspector already models;
            // structural spawn/destroy/load-prefab fall back to the tool's raw path.
            return false;
        }
        return false;
    }
}
