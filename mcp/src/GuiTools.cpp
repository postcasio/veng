#include "GuiTools.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Gui/Document.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Gui/Surface.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief How deep gui.inspect descends when the caller names no depth.
        ///
        /// Deep enough to reach the controls inside a panel, shallow enough that a document holding
        /// a long authored pool does not spend an agent's context on rows it did not ask about.
        /// The caller widens it, or narrows the walk to one subtree with `id`.
        constexpr i64 DefaultDepth = 6;

        /// @brief The most elements one gui.inspect reports, whatever the depth asks for.
        ///
        /// A volume convention for the single trusted local client, not a defense: a pool of a
        /// hundred rows times its cells is a large document, and a truncated answer that says it
        /// truncated is more useful than one that fills a context window.
        constexpr usize MaxElements = 2000;

        [[nodiscard]] string_view KindName(const Gui::ElementKind kind)
        {
            switch (kind)
            {
            case Gui::ElementKind::Panel:
                return "Panel";
            case Gui::ElementKind::Text:
                return "Text";
            case Gui::ElementKind::Image:
                return "Image";
            case Gui::ElementKind::Button:
                return "Button";
            case Gui::ElementKind::Checkbox:
                return "Checkbox";
            case Gui::ElementKind::Slider:
                return "Slider";
            case Gui::ElementKind::ProgressBar:
                return "ProgressBar";
            case Gui::ElementKind::TextInput:
                return "TextInput";
            case Gui::ElementKind::ScrollView:
                return "ScrollView";
            case Gui::ElementKind::List:
                return "List";
            default:
                break;
            }
            return "Element";
        }

        /// @brief One live document found in the scene, with what names it.
        struct FoundDocument
        {
            Gui::Document* Doc = nullptr;
            Entity Owner = Entity::Null;
            string_view Kind;
            string Name;
        };

        /// @brief Every document the scene presents, surfaces first then overlays.
        ///
        /// The two component kinds are the whole set: a surface maps a document onto a world mesh
        /// and an overlay attaches one to the viewport's layer stack, and a document reachable by
        /// neither is not being presented. A component whose document has not been instantiated yet
        /// is skipped rather than reported as an empty one.
        [[nodiscard]] vector<FoundDocument> FindDocuments(Scene& scene)
        {
            vector<FoundDocument> found;
            const auto nameOf = [&scene](const Entity entity)
            {
                const auto* const name = std::as_const(scene).TryGet<Name>(entity);
                return name != nullptr ? name->Value : string{};
            };
            for (auto [entity, surface] : scene.View<GuiSurface>())
            {
                if (Gui::Document* const doc = surface.GetDocument(); doc != nullptr)
                {
                    found.push_back(FoundDocument{
                        .Doc = doc, .Owner = entity, .Kind = "surface", .Name = nameOf(entity)});
                }
            }
            for (auto [entity, overlay] : scene.View<GuiOverlay>())
            {
                if (Gui::Document* const doc = overlay.GetDocument(); doc != nullptr)
                {
                    found.push_back(FoundDocument{
                        .Doc = doc, .Owner = entity, .Kind = "overlay", .Name = nameOf(entity)});
                }
            }
            return found;
        }

        /// @brief Resolves the document a request names, or the only one when it names none.
        ///
        /// A request carrying no `entity` reads the first document found, which is the whole answer
        /// on a scene presenting one. Naming an entity that presents no document is an error rather
        /// than a silent fall-through to a different document than the caller meant.
        [[nodiscard]] Result<FoundDocument> ResolveDocument(Scene& scene, const Json& args)
        {
            const vector<FoundDocument> found = FindDocuments(scene);
            if (found.empty())
            {
                return std::unexpected(string("this world presents no gui document"));
            }
            if (!args.is_object() || !args.contains("entity") || !args["entity"].is_number())
            {
                return found.front();
            }
            const auto index = args["entity"].get<i64>();
            const auto match = std::ranges::find_if(found, [index](const FoundDocument& candidate)
                                                    { return candidate.Owner.Index == index; });
            if (match == found.end())
            {
                return std::unexpected(string("entity presents no gui document"));
            }
            return *match;
        }

        /// @brief One element as JSON: what names it, whether it draws, and the box it solved to.
        ///
        /// `rect` is the element's **border box** in document points, straight off the layout pass —
        /// which is the reading these tools exist for. `text` is the run the element paints, elided
        /// past a length no layout question needs.
        [[nodiscard]] Json DescribeElement(const Gui::Element& element)
        {
            Json node{{"kind", KindName(element.Kind)},
                      {"visible", element.Visible},
                      {"rect",
                       {{"x", element.Layout.Min.x},
                        {"y", element.Layout.Min.y},
                        {"w", element.Layout.Size.x},
                        {"h", element.Layout.Size.y}}}};
            if (!element.Id.empty())
            {
                node["id"] = element.Id;
            }
            if (!element.Classes.empty())
            {
                node["classes"] = element.Classes;
            }
            if (!element.Text.empty())
            {
                constexpr usize TextCap = 120;
                node["text"] =
                    element.Text.size() > TextCap ? element.Text.substr(0, TextCap) : element.Text;
            }
            return node;
        }

        /// @brief Walks a subtree into JSON, bounded by depth and by the element cap.
        ///
        /// The two bounds are reported rather than applied silently: a walk that stopped short says
        /// so on the node it stopped at, so a caller can tell a leaf from a floor it hit and widen
        /// the request rather than conclude the tree ends there.
        void DescribeSubtree(const Gui::Element& element, const i64 depth, usize& budget, Json& out)
        {
            out = DescribeElement(element);
            ++budget;
            if (element.Children.empty())
            {
                return;
            }
            if (depth <= 0)
            {
                out["children_elided"] = element.Children.size();
                return;
            }
            Json children = Json::array();
            for (const Gui::Element* child : element.Children)
            {
                if (child == nullptr)
                {
                    continue;
                }
                if (budget >= MaxElements)
                {
                    out["children_elided"] = element.Children.size() - children.size();
                    break;
                }
                Json node;
                DescribeSubtree(*child, depth - 1, budget, node);
                children.push_back(std::move(node));
            }
            if (!children.empty())
            {
                out["children"] = std::move(children);
            }
        }
    }

    void RegisterGuiTools(McpServer& server, const McpHost& host)
    {
        // gui.list_documents — what interfaces this world is presenting, and how big each is.
        {
            McpTool tool;
            tool.Name = "gui.list_documents";
            tool.Description =
                "Lists the gui documents the current world presents (owning entity, surface or "
                "overlay, canvas size, element count). Pass an entity id from here to gui.inspect "
                "to read a specific one.";
            tool.InputSchemaJson = R"({"type":"object","properties":{}})";
            tool.Handler = [&host](string_view) -> Result<string>
            {
                Scene* scene = host.CurrentWorld ? host.CurrentWorld() : nullptr;
                if (scene == nullptr)
                {
                    return Json{{"documents", Json::array()}}.dump();
                }
                Json documents = Json::array();
                for (const FoundDocument& found : FindDocuments(*scene))
                {
                    const Gui::Element& root = found.Doc->Root();
                    documents.push_back(
                        Json{{"entity", found.Owner.Index},
                             {"kind", found.Kind},
                             {"name", found.Name},
                             {"canvas", {{"w", root.Layout.Size.x}, {"h", root.Layout.Size.y}}}});
                }
                return Json{{"documents", std::move(documents)}}.dump();
            };
            server.RegisterTool(std::move(tool));
        }

        // gui.inspect — the solved element tree, which is the reading a screenshot cannot give.
        {
            McpTool tool;
            tool.Name = "gui.inspect";
            tool.Description =
                "Dumps a gui document's solved element tree as JSON: id, classes, kind, "
                "visibility, "
                "text, and the layout rect in document points. Arguments: { entity?: <id from "
                "gui.list_documents, default the first>, id?: <element id to root the walk at>, "
                "depth?: <levels to descend, default 6> }.";
            tool.InputSchemaJson = R"({"type":"object","properties":{"entity":{"type":"integer"},)"
                                   R"("id":{"type":"string"},"depth":{"type":"integer"}}})";
            tool.Handler = [&host](string_view argsJson) -> Result<string>
            {
                const Json args = Json::parse(argsJson, nullptr, false);
                Scene* scene = host.CurrentWorld ? host.CurrentWorld() : nullptr;
                if (scene == nullptr)
                {
                    return std::unexpected(string("no world is loaded"));
                }
                const Result<FoundDocument> found = ResolveDocument(*scene, args);
                if (!found)
                {
                    return std::unexpected(found.error());
                }

                // The walk's root: a named element where the caller gave one, so a long document is
                // read a panel at a time rather than whole.
                const Gui::Element* root = &found->Doc->Root();
                if (args.is_object() && args.contains("id") && args["id"].is_string())
                {
                    const auto id = args["id"].get<string>();
                    root = std::as_const(*found->Doc).FindById(id);
                    if (root == nullptr)
                    {
                        return std::unexpected("no element carries the id '" + id + "'");
                    }
                }

                const i64 depth =
                    args.is_object() && args.contains("depth") && args["depth"].is_number()
                        ? args["depth"].get<i64>()
                        : DefaultDepth;
                usize budget = 0;
                Json tree;
                DescribeSubtree(*root, depth, budget, tree);
                return Json{{"entity", found->Owner.Index},
                            {"kind", found->Kind},
                            {"elements", budget},
                            {"root", std::move(tree)}}
                    .dump();
            };
            server.RegisterTool(std::move(tool));
        }
    }
}
