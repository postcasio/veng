#include <Veng/Renderer/ViewportCompositor.h>

#include <Veng/Assert.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Log.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GatherPass.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneCapture.h>
#include <Veng/Renderer/SwapChainCompositePass.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Window.h>

#include "CaptureRotation.h"

#include <algorithm>

#include <glm/glm.hpp>

namespace Veng::Renderer
{
    ViewportCompositor::ViewportCompositor(Context& context) : m_Context(context) {}

    ViewportCompositor::~ViewportCompositor()
    {
        // Release the tail's GPU resources while the context is still live. The placement cache retains
        // a Ref to each Presented viewport's output view for change-detection; clearing it here — after
        // the managed viewports have already dropped, since they are declared after the compositor —
        // releases those outputs so the images retire rather than outliving the context's allocator.
        m_CompositeGraph.reset();
        m_Composite.reset();
        m_GatherGraph.reset();
        m_Gather.reset();
        m_GatheredPlacements.clear();
    }

    void ViewportCompositor::InitializeTail(AssetManager& assets, ImGuiLayer& imgui)
    {
        m_Gather = GatherPass::Create({
            .Context = m_Context,
            .Assets = assets,
            .Extent = m_Context.GetSwapChainExtent(),
        });

        m_Composite = SwapChainCompositePass::Create({
            .Context = m_Context,
            .ImGui = imgui,
            .Assets = assets,
            .SceneSource = m_Gather->GetOutput(),
            .SwapChainFormat = m_Context.GetSwapChainFormat(),
            .ColorSpace = m_Context.GetActiveDisplayColorSpace(),
        });

        const auto compileGather = [this]
        {
            RenderGraph graph(m_Context);
            return m_Gather->Compile(graph);
        };
        const auto compileComposite = [this]
        {
            RenderGraph graph(m_Context);
            const ResourceId swapId = graph.Import("SwapChain");
            return m_Composite->Compile(graph, swapId);
        };

        // Swapchain recreation invalidates the baked extent and may re-negotiate the surface's
        // format/color space (a window moved to a display with different HDR support); re-target
        // the composite before recompiling.
        m_Context.AddSwapChainInvalidationCallback(
            [this, compileGather, compileComposite]
            {
                m_Gather->Resize(m_Context.GetSwapChainExtent());
                m_Composite->SetSceneSource(m_Gather->GetOutput());
                // The ImGui layer's invalidation callback (registered earlier, so it ran first)
                // recreated its offscreen image; re-point the composite at it or it samples the
                // retired one (old size → squished, stale content → frozen overlay).
                m_Composite->RefreshImGuiSource();
                m_Composite->SetSwapChainTarget(m_Context.GetSwapChainFormat(),
                                                m_Context.GetActiveDisplayColorSpace());
                m_GatherGraph = compileGather();
                m_CompositeGraph = compileComposite();
            });

        m_GatherGraph = compileGather();
        m_CompositeGraph = compileComposite();
    }

    void ViewportCompositor::RegisterViewport(Viewport& viewport)
    {
        VE_ASSERT(std::ranges::find(m_Viewports, &viewport) == m_Viewports.end(),
                  "Viewport is already registered to the compositor's drive-list");

        m_Viewports.emplace_back(&viewport);
        viewport.AttachToDriveList(m_Viewports);
    }

    void ViewportCompositor::RegisterCapture(SceneCapture& capture)
    {
        VE_ASSERT(std::ranges::find(m_Captures, &capture) == m_Captures.end(),
                  "SceneCapture is already registered to the compositor's drive-list");

        m_Captures.emplace_back(&capture);
        capture.AttachToDriveList(m_Captures);
    }

    void ViewportCompositor::RenderRegistered(CommandBuffer& cmd)
    {
        // Scene captures render first, so a material sampling a capture's output reads this frame's
        // result during the viewport renders that follow. Rendering first is also what puts them
        // ahead of the viewports in the frame's view budget, so the drive spends only what it can
        // leave the viewports: a missing reflection is a blemish, a viewport that could not claim a
        // slot is a stale window. One slot per registered viewport is the floor, which a viewport
        // whose sky re-bakes this frame still exceeds — that bake gives way instead and retries.
        DriveCaptures(cmd);

        // Then every registered viewport in registration order (each does its own Execute + Sample
        // barrier), so viewport outputs are in Sample layout before a later consumer samples them.
        for (Viewport* viewport : m_Viewports)
        {
            // A per-viewport scope named by the viewport's id: a multi-viewport app reads as
            // separate entries rather than one summed bar.
            const string label = "Viewport " + std::to_string(viewport->GetId().Value);
            VE_PROFILE_SCOPE_DYNAMIC(label);
            viewport->Render(cmd);
        }
    }

    void ViewportCompositor::DriveCaptures(CommandBuffer& cmd)
    {
        if (m_Captures.empty())
        {
            return;
        }

        const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
        const u32 reserved = static_cast<u32>(m_Viewports.size());
        const usize count = m_Captures.size();

        // Round-robin from where the last budget-limited frame stopped, so a capture set larger than
        // the budget refreshes in turn. A capture renders one cube face per driven frame anyway, so
        // what an over-budget frame costs is refresh latency, not a capture that never renders.
        m_CaptureCursor %= count;
        usize driven = 0;
        while (driven < count)
        {
            // A capture with no fresh view records nothing and claims no slot, so the budget is
            // measured against the registry each step rather than assumed one per capture — a set of
            // settled on-demand captures displaces nothing.
            if (!CaptureDriveHasRoom(registry.GetRemainingViews(), reserved))
            {
                break;
            }
            m_Captures[CaptureDriveIndex(m_CaptureCursor, driven, count)]->Render(cmd);
            ++driven;
        }

        if (driven < count && !m_WarnedCaptureBudget)
        {
            m_WarnedCaptureBudget = true;
            Log::Warn(
                "ViewportCompositor: the frame's {} view slots cover {} viewport(s) and {} of "
                "{} scene captures; the rest hold their last map and refresh on later frames.",
                BindlessRegistry::MaxViewsPerFrame, reserved, driven, count);
        }
        m_CaptureCursor = NextCaptureCursor(m_CaptureCursor, driven, count);
    }

    void ViewportCompositor::Composite(CommandBuffer& cmd)
    {
        if (!m_Gather)
        {
            return;
        }

        // Assemble the registered Presented viewports into the gather target, each into its own
        // region. Zero placements composites ImGui over a clear (the editor's case).
        vector<CompositePlacement> placements;
        for (const Viewport* viewport : m_Viewports)
        {
            if (viewport->GetRole() == ViewportRole::Presented)
            {
                placements.emplace_back(CompositePlacement{
                    .Texture = viewport->GetOutput(),
                    .Region = viewport->GetRegion(),
                });
            }
        }

        // Rebind only when the placement set changed (output identity or region), so a steady
        // frame issues no bindless re-registration.
        const auto samePlacement = [](const CompositePlacement& a, const CompositePlacement& b)
        {
            return a.Texture == b.Texture && a.Region.Offset == b.Region.Offset &&
                   a.Region.Extent == b.Region.Extent;
        };
        if (!std::ranges::equal(placements, m_GatheredPlacements, samePlacement))
        {
            m_Gather->SetPlacements(placements);
            m_GatheredPlacements = std::move(placements);
        }

        m_Gather->Execute(cmd, *m_GatherGraph);

        // The composite samples the assembly target outside the graph; transition it.
        cmd.PrepareForAccess(m_Gather->GetOutput(), AccessKind::SampleGraphics);

        m_Composite->Execute(cmd, *m_CompositeGraph, m_Context.GetCurrentSwapChainImageView());
    }

    ViewportRegion ViewportCompositor::ResolveLayout(const ViewportLayout& layout) const
    {
        const vec2 renderExtent = vec2(m_Context.GetRenderExtent());
        const ivec2 offset = ivec2(glm::round(layout.Offset * renderExtent));
        const uvec2 extent = uvec2(glm::round(layout.Extent * renderExtent));
        return {.Offset = offset, .Extent = extent};
    }

    void ViewportCompositor::ResolveTrackingLayouts()
    {
        // Screen-space Gui documents lay out in logical points while the region is framebuffer
        // pixels, so a window-tracking viewport's UI scale follows the window content scale: authored
        // px render at logical size on a HiDPI display. Headless borrows no window and stamps 1.0.
        const f32 uiScale =
            m_Context.IsHeadless() ? 1.0f : m_Context.GetWindow().GetContentScale().x;

        for (Viewport* viewport : m_Viewports)
        {
            if (const optional<ViewportLayout>& layout = viewport->GetLayout())
            {
                viewport->SetRegion(ResolveLayout(*layout));
                viewport->SetUiScale(uiScale);
            }
        }
    }
}
