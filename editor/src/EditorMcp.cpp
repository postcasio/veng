#include "EditorMcp.h"

#include "AssetEditorPanel.h"
#include "AssetSourceIndex.h"
#include "CommandStack.h"
#include "EditorCommand.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

// The shared viewport-capture path (Download -> tonemap -> PNG -> base64) behind render.screenshot,
// reused by editor.screenshot_panel. An internal veng::mcp source header reached through the
// editor's private include of mcp/src.
#include "ViewportCapture.h"

#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/Viewport.h>

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

#include <algorithm>
#include <charconv>

namespace VengEditor
{
    using namespace Veng;
    using Veng::Mcp::McpMutation;
    using Veng::Mcp::McpMutationKind;
    using Json = nlohmann::json;

    namespace
    {
        /// @brief Resolves a panel by title among the host's open panels, or null.
        ///
        /// Compares marker-stripped titles (see StripUnsavedMarker), so a name captured from
        /// editor.list_panels keeps resolving after an edit marks the document dirty.
        EditorPanel* FindPanel(const EditorMcpHost& host, const string& title)
        {
            const string_view wanted = StripUnsavedMarker(title);
            // The returned panel is mutated by the caller (GetInspectables / OnInspectableChanged
            // are non-const), so the loop pointee stays mutable.
            for (EditorPanel* panel : host.Panels()) // NOLINT(misc-const-correctness)
            {
                if (panel != nullptr && StripUnsavedMarker(panel->GetTitle()) == wanted)
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
                    // The marker-stripped title is the stable panel address every by-title tool
                    // resolves; the raw display title gains a '*' while the document is dirty.
                    panels.push_back(Json{{"title", string{StripUnsavedMarker(panel->GetTitle())}},
                                          {"kind", PanelKind(*panel)},
                                          {"focused", panel == focused},
                                          {"dirty", panel->GetTitle().starts_with('*')},
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

    namespace
    {
        /// @brief Default page size for editor.list_assets when the caller omits `limit`.
        constexpr u32 ListAssetsDefaultLimit = 200;

        /// @brief Parses the optional `limit` argument, clamped to the page cap.
        u32 ParseLimit(const Json& args)
        {
            if (!args.is_object() || !args.contains("limit") || !args["limit"].is_number())
            {
                return ListAssetsDefaultLimit;
            }
            const i64 requested = args["limit"].get<i64>();
            if (requested <= 0)
            {
                return ListAssetsDefaultLimit;
            }
            return static_cast<u32>(std::min<i64>(requested, ListAssetsDefaultLimit));
        }

        /// @brief Parses the opaque `cursor` (the resume offset into the sorted id list), defaulting to 0.
        u32 ParseCursor(const Json& args)
        {
            if (!args.is_object() || !args.contains("cursor"))
            {
                return 0;
            }
            const Json& cursor = args["cursor"];
            if (cursor.is_number())
            {
                const i64 value = cursor.get<i64>();
                return value > 0 ? static_cast<u32>(value) : 0;
            }
            if (cursor.is_string())
            {
                const string text = cursor.get<string>();
                u32 value = 0;
                const auto [ptr, ec] =
                    std::from_chars(text.data(), text.data() + text.size(), value);
                return ec == std::errc{} ? value : 0;
            }
            return 0;
        }

        /// @brief Resolves an AssetId argument (decimal string or number) from a request, or a false optional.
        optional<AssetId> ParseAssetId(const Json& args)
        {
            if (!args.is_object() || !args.contains("asset"))
            {
                return std::nullopt;
            }
            const Json& asset = args["asset"];
            if (asset.is_string())
            {
                const string text = asset.get<string>();
                u64 value = 0;
                const auto [ptr, ec] =
                    std::from_chars(text.data(), text.data() + text.size(), value);
                if (ec != std::errc{} || ptr != text.data() + text.size())
                {
                    return std::nullopt;
                }
                return AssetId{value};
            }
            if (asset.is_number_unsigned())
            {
                return AssetId{asset.get<u64>()};
            }
            return std::nullopt;
        }
    }

    void RegisterEditorHostTools(Mcp::McpServer& server, const EditorMcpHost& host)
    {
        // editor.open_asset — opens the registered editor for an asset (the "open the editor for
        // this asset" verb), resolving its type from the source index.
        {
            Mcp::McpTool tool;
            tool.Name = "editor.open_asset";
            tool.Description =
                "Opens the registered editor for an asset. Argument: { asset: <AssetId> } (a "
                "decimal id string or number). Errors when the id is unknown to the project or its "
                "type has no editor.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["asset"],"properties":{"asset":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const optional<AssetId> id = ParseAssetId(args);
                if (!id)
                {
                    return std::unexpected(string("expected { asset: <AssetId> }"));
                }
                if (!host.OpenAsset)
                {
                    return std::unexpected(string("opening assets is unavailable"));
                }
                if (!host.OpenAsset(*id))
                {
                    return std::unexpected(
                        fmt::format("asset {} is unknown or its type has no editor", id->Value));
                }
                return Json{{"opened", std::to_string(id->Value)}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // editor.set_panel_visible — the programmatic Window-menu toggle (show/hide a tool panel,
        // open/close a document).
        {
            Mcp::McpTool tool;
            tool.Name = "editor.set_panel_visible";
            tool.Description = "Shows or hides an open panel by title. Argument: { panel: <title>, "
                               "visible: bool }. "
                               "A tool panel hides and can be reshown; a document panel closes "
                               "when hidden. Errors "
                               "when no open panel matches the title.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["panel","visible"],"properties":)"
                R"({"panel":{"type":"string"},"visible":{"type":"boolean"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                if (!args.is_object() || !args.contains("panel") || !args["panel"].is_string() ||
                    !args.contains("visible") || !args["visible"].is_boolean())
                {
                    return std::unexpected(string("expected { panel: <title>, visible: bool }"));
                }
                if (!host.SetPanelVisible)
                {
                    return std::unexpected(string("panel visibility is unavailable"));
                }
                const string title = args["panel"].get<string>();
                const bool visible = args["visible"].get<bool>();
                if (!host.SetPanelVisible(title, visible))
                {
                    return std::unexpected(fmt::format("no open panel titled '{}'", title));
                }
                return Json{{"panel", title}, {"visible", visible}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // editor.list_assets — project-wide asset inspection over the source index, paginated and
        // optionally filtered by type.
        {
            Mcp::McpTool tool;
            tool.Name = "editor.list_assets";
            tool.Description =
                "Lists the project's assets from the source index: each id, name, type, and source "
                "path. Argument: { type?: <AssetType name>, limit?, cursor? }. 'type' filters by a "
                "canonical type name (texture, material, prefab, level, …). Paginated: returns "
                "{ assets, nextCursor? } — page the tail through nextCursor.";
            tool.InputSchemaJson = R"({"type":"object","properties":{"type":{"type":"string"},)"
                                   R"("limit":{"type":"integer"},"cursor":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);

                optional<AssetType> filter;
                if (args.is_object() && args.contains("type") && args["type"].is_string())
                {
                    const string typeName = args["type"].get<string>();
                    filter = ParseAssetType(typeName);
                    if (!filter)
                    {
                        return std::unexpected(fmt::format("unknown asset type '{}'", typeName));
                    }
                }

                const AssetSourceIndex* sources = host.AssetSources ? host.AssetSources() : nullptr;
                if (sources == nullptr)
                {
                    return Json{{"assets", Json::array()}}.dump();
                }

                // Collect the matching (id, entry) pairs, then sort by id so the cursor is a stable
                // offset across calls (the index enumerates in unspecified order).
                struct Row
                {
                    AssetId Id;
                    const AssetSourceIndex::Entry* Entry;
                };
                vector<Row> rows;
                sources->ForEachEntry(
                    [&](AssetId id, const AssetSourceIndex::Entry& entry)
                    {
                        if (!filter || entry.Type == *filter)
                        {
                            rows.push_back({.Id = id, .Entry = &entry});
                        }
                    });
                std::ranges::sort(rows, [](const Row& a, const Row& b)
                                  { return a.Id.Value < b.Id.Value; });

                const u32 limit = ParseLimit(args);
                const u32 cursor = ParseCursor(args);
                Json assets = Json::array();
                usize index = cursor;
                for (; index < rows.size() && assets.size() < limit; ++index)
                {
                    const Row& row = rows[index];
                    assets.push_back(Json{{"id", std::to_string(row.Id.Value)},
                                          {"name", row.Entry->RelativeSource.string()},
                                          {"type", ToString(row.Entry->Type)},
                                          {"source", row.Entry->Source.string()}});
                }

                Json result{{"assets", std::move(assets)}};
                if (index < rows.size())
                {
                    result["nextCursor"] = std::to_string(index);
                }
                return result.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // editor.screenshot_panel — a named panel's Offscreen viewport captured through the same
        // Download -> PNG -> base64 path render.screenshot uses.
        {
            Mcp::McpTool tool;
            tool.Name = "editor.screenshot_panel";
            tool.Description =
                "Captures a named editor panel's rendered scene as a PNG image (tonemapped 8-bit "
                "scene color). Argument: { panel: <title> }. Errors when the title names no open "
                "panel or the panel renders no scene. Returns an image content block.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["panel"],"properties":{"panel":{"type":"string"}}})";
            tool.ReturnsContentBlocks = true;
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                if (!args.is_object() || !args.contains("panel") || !args["panel"].is_string())
                {
                    return std::unexpected(string("expected { panel: <title> }"));
                }
                const string title = args["panel"].get<string>();
                Renderer::Viewport* viewport =
                    host.PanelViewport ? host.PanelViewport(title) : nullptr;
                if (viewport == nullptr)
                {
                    return std::unexpected(
                        fmt::format("no open panel titled '{}' renders a scene", title));
                }
                return Mcp::CaptureViewportContentBlocks(*viewport);
            };
            server.RegisterTool(std::move(tool));
        }

        // editor.request_cook — the editor-only cook-on-demand path (fire-and-poll), distinct from
        // the runtime world.load_prefab which never cooks.
        {
            Mcp::McpTool tool;
            tool.Name = "editor.request_cook";
            tool.Description =
                "Cooks a project asset on demand and hot-reloads it, off the render thread. "
                "Argument: { asset: <AssetId> }. Returns { status: \"started\" } immediately — "
                "poll "
                "editor.cook_status for completion. Errors when the id is unknown or "
                "cook-on-demand "
                "is unconfigured.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["asset"],"properties":{"asset":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const optional<AssetId> id = ParseAssetId(args);
                if (!id)
                {
                    return std::unexpected(string("expected { asset: <AssetId> }"));
                }
                if (!host.RequestCook)
                {
                    return std::unexpected(string("cook-on-demand is unavailable"));
                }
                const VoidResult kicked = host.RequestCook(*id);
                if (!kicked)
                {
                    return std::unexpected(kicked.error());
                }
                return Json{{"status", "started"}, {"asset", std::to_string(id->Value)}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // editor.cook_status — the companion poll for editor.request_cook.
        {
            Mcp::McpTool tool;
            tool.Name = "editor.cook_status";
            tool.Description =
                "Reports the latest cook status of an asset requested through editor.request_cook. "
                "Argument: { asset: <AssetId> }. Returns { status } — \"running\", \"ok\", or an "
                "error message — or { status: \"none\" } when no cook of this id was requested.";
            tool.InputSchemaJson =
                R"({"type":"object","required":["asset"],"properties":{"asset":{"type":"string"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                const optional<AssetId> id = ParseAssetId(args);
                if (!id)
                {
                    return std::unexpected(string("expected { asset: <AssetId> }"));
                }
                const optional<string> status =
                    host.CookStatus ? host.CookStatus(*id) : std::nullopt;
                return Json{{"asset", std::to_string(id->Value)},
                            {"status", status.value_or(string("none"))}}
                    .dump();
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
