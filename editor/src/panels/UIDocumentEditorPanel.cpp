#include "UIDocumentEditorPanel.h"

#include <Veng/Application.h>
#include <Veng/Gui/Element.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <Veng/Log.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Time.h>
#include <Veng/UI/UI.h>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        constexpr uvec2 CanvasExtent{512, 384};

        string_view KindName(Gui::ElementKind kind)
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
            }
            return "Element";
        }

        // A one-line label for an outline row: the kind plus any id/first-class tag.
        string OutlineLabel(const Gui::Element& element)
        {
            string label(KindName(element.Kind));
            if (!element.Id.empty())
            {
                label += fmt::format(" #{}", element.Id);
            }
            else if (!element.Classes.empty())
            {
                label += fmt::format(" .{}", element.Classes.front());
            }
            return label;
        }
    }

    UIDocumentEditorPanel::UIDocumentEditorPanel(AssetId id, path sourcePath, Application& app,
                                                 AssetManager& assets, ImGuiLayer& imgui,
                                                 CookDriver cook)
        : m_Id(id), m_SourcePath(std::move(sourcePath)), m_App(app),
          m_Context(app.GetRenderContext()), m_Assets(assets), m_ImGui(imgui),
          m_Cook(std::move(cook))
    {
        m_Title = fmt::format("UI Document: {}", m_SourcePath.filename().string());

        // The canvas composites the document over an empty scene: a viewport with a null-World
        // ViewState renders a cleared target, and the attached document's GuiScenePass blends the
        // UI over it. So the canvas needs no meshes, camera, or lights — only a viewport.
        m_Types = CreateUnique<TypeRegistry>();
        RegisterBuiltinTypes(*m_Types);
        m_Scene = Scene::Create(*m_Types);

        m_Viewport = Renderer::Viewport::Create({
            .Context = m_Context,
            .Assets = m_Assets,
            .Region = {.Offset = {0, 0}, .Extent = CanvasExtent},
            .ColorFormat = m_Context.GetOutputFormat(),
            .Role = Renderer::ViewportRole::Offscreen,
            // Render only while the editor tab draws; a hidden tab pushes no ViewState.
            .RenderOnDemand = true,
        });
        m_App.RegisterViewport(*m_Viewport);

        m_Sampler = Renderer::Sampler::Create(
            m_Context, {
                           .Name = "UI Document Canvas Sampler",
                           .AddressModeU = Renderer::AddressMode::ClampToEdge,
                           .AddressModeV = Renderer::AddressMode::ClampToEdge,
                           .AddressModeW = Renderer::AddressMode::ClampToEdge,
                       });

        TriggerCook();
    }

    UIDocumentEditorPanel::~UIDocumentEditorPanel()
    {
        // Drop the document before the viewport so it self-detaches from the layer stack, then the
        // viewport (which unregisters from the app drive-list through its own destructor).
        m_Document.reset();
        m_CanvasTexture.reset();
        m_Sampler.reset();
        m_Viewport.reset();
        m_Scene.reset();
        m_Types.reset();
    }

    void UIDocumentEditorPanel::TriggerCook()
    {
        if (m_Cooking)
        {
            return;
        }

        m_Cooking = true;
        m_CookError.reset();

        m_Cook({.SourcePath = m_SourcePath, .TargetId = m_Id, .Type = AssetType::UIDocument},
               [this](Result<MountHandle> mount)
               {
                   m_Cooking = false;
                   if (!mount)
                   {
                       m_CookError = mount.error();
                       return;
                   }

                   // Swap the mount behind the stable handle, then force a re-resolve: drop the
                   // prior document (and its recipe handle) and collect garbage so the stale cache
                   // entry evicts, letting the re-fetch resolve the recooked recipe through the new
                   // shadow mount. The recipe is small CPU data, so LoadSync resolves inline;
                   // RebuildDocument runs next OnUI frame once m_DocumentDirty is observed.
                   m_Mount = std::move(*mount);
                   m_Document.reset();
                   m_Handle = {};
                   m_Assets.CollectGarbage();

                   const AssetResult<AssetHandle<Gui::UIDocument>> handle =
                       m_Assets.LoadSync<Gui::UIDocument>(m_Id);
                   if (!handle)
                   {
                       m_CookError = handle.error().Detail;
                       return;
                   }
                   m_Handle = *handle;
                   m_DocumentDirty = true;
               });
    }

    void UIDocumentEditorPanel::RebuildDocument()
    {
        if (!m_Handle.IsLoaded())
        {
            return;
        }

        // Dropping the prior document self-detaches it from the viewport's layer stack.
        m_Document.reset();
        m_Selected = static_cast<usize>(-1);
        m_SelectedElement = nullptr;

        m_Document = Gui::Document::Instantiate(*m_Handle.Get(), m_Assets);
        m_Viewport->AttachDocument(*m_Document);
    }

    void UIDocumentEditorPanel::DrawOutline(Gui::Element& element, u32& index)
    {
        const usize self = index;
        ++index;

        const bool selected = self == m_Selected;
        if (UI::Selectable(OutlineLabel(element), selected))
        {
            m_Selected = self;
            m_SelectedElement = &element;
        }

        if (!element.Children.empty())
        {
            UI::Indent();
            for (Gui::Element* child : element.Children)
            {
                DrawOutline(*child, index);
            }
            UI::Unindent();
        }
    }

    void UIDocumentEditorPanel::DrawStyleInspector()
    {
        if (m_SelectedElement == nullptr)
        {
            UI::TextDisabled("Select an element in the outline.");
            return;
        }

        const Gui::Element& element = *m_SelectedElement;
        const Gui::Style& style = element.ComputedStyle;

        UI::Text(fmt::format("Kind: {}", KindName(element.Kind)));
        if (!element.Id.empty())
        {
            UI::Text(fmt::format("Id: {}", element.Id));
        }
        if (!element.Classes.empty())
        {
            string classes;
            for (const string& cls : element.Classes)
            {
                classes += (classes.empty() ? "" : " ") + cls;
            }
            UI::Text(fmt::format("Classes: {}", classes));
        }
        if (element.Kind == Gui::ElementKind::Text && !element.Text.empty())
        {
            UI::Text(fmt::format("Text: {}", element.Text));
        }

        UI::Separator();

        // The resolved layout rect (document-space pixels), filled by the last Solve.
        UI::Text(fmt::format("Layout: {:.0f}, {:.0f}  {:.0f} x {:.0f}", element.Layout.Min.x,
                             element.Layout.Min.y, element.Layout.Size.x, element.Layout.Size.y));

        UI::Separator();

        UI::TextDisabled("Resolved style");
        UI::Text(fmt::format("Background: {:.2f} {:.2f} {:.2f} {:.2f}", style.Background.r,
                             style.Background.g, style.Background.b, style.Background.a));
        if (element.Kind == Gui::ElementKind::Text)
        {
            UI::Text(fmt::format("Text color: {:.2f} {:.2f} {:.2f} {:.2f}", style.TextColor.r,
                                 style.TextColor.g, style.TextColor.b, style.TextColor.a));
            UI::Text(fmt::format("Font size: {:.1f}px", style.TextSize));
        }
        UI::Text(fmt::format("Opacity: {:.2f}", style.Opacity));
        UI::Text(fmt::format("Corner radius: {:.1f}", style.Radii.TopLeft));
        UI::Text(fmt::format("Padding: {:.0f} {:.0f} {:.0f} {:.0f}", style.Padding.Left,
                             style.Padding.Top, style.Padding.Right, style.Padding.Bottom));
        UI::Text(fmt::format("Margin: {:.0f} {:.0f} {:.0f} {:.0f}", style.Margin.Left,
                             style.Margin.Top, style.Margin.Right, style.Margin.Bottom));
    }

    void UIDocumentEditorPanel::OnUI()
    {
        if (m_DocumentDirty)
        {
            RebuildDocument();
            m_DocumentDirty = false;
            // A fresh output image invalidates the canvas texture.
            m_CanvasTexture.reset();
        }

        // Drive the canvas viewport this frame: the empty scene renders a cleared target and the
        // attached document composites over it. Pushing the state each visible frame keeps the
        // RenderOnDemand viewport rendering; a hidden tab stops (no OnUI, no push).
        m_Viewport->SetViewState({.World = m_Scene.get(), .Delta = Time::GetDeltaTime()});

        if (!m_CanvasTexture && m_Viewport->GetOutput())
        {
            m_CanvasTexture = m_ImGui.CreateTexture(*m_Sampler, *m_Viewport->GetOutput());
        }

        if (m_CanvasTexture)
        {
            const f32 aspect = static_cast<f32>(CanvasExtent.y) / static_cast<f32>(CanvasExtent.x);
            const f32 width = std::max(UI::ContentRegionAvail().x, 16.0f);
            UI::Image(m_CanvasTexture, {width, width * aspect});
        }
        else
        {
            UI::Text(m_Cooking ? "Cooking..." : "Loading...");
        }

        if (m_CookError)
        {
            UI::TextColored({0.9f, 0.3f, 0.3f, 1.0f}, fmt::format("Cook error: {}", *m_CookError));
        }

        UI::Separator();

        if (UI::Button("Recook"))
        {
            TriggerCook();
        }
        UI::Tooltip("Re-cook the source and hot-reload the document behind its stable handle");

        UI::SeparatorText("Outline");
        if (m_Document)
        {
            u32 index = 0;
            DrawOutline(m_Document->Root(), index);
        }
        else
        {
            UI::TextDisabled("No document.");
        }

        UI::SeparatorText("Style");
        DrawStyleInspector();
    }
}
