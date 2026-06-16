#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneRenderer.h>

// The reusable-pass-unit layer SceneRenderer composes its pipeline from. A
// ScenePass is a self-contained pipeline stage: it knows how to size its own
// resources, declare its reads/writes, and record — never what feeds it. The
// renderer owns the wiring (which pass reads whose target); each pass owns itself.
//
// A ScenePass is distinct from RenderGraph::Pass: it contributes one or more
// RenderGraph passes into the renderer's single internal graph, it is not one.
namespace Veng::Renderer
{
    class CommandBuffer;

    // The record-time context a ScenePass callback receives: the RenderGraph
    // record-time channel plus this frame's SceneView, typed back from the graph's
    // opaque user pointer. SceneRenderer is the Scene-aware layer that owns this
    // wrapper and guarantees the pointer is set and the right type on every Execute.
    class ScenePassContext
    {
    public:
        [[nodiscard]] CommandBuffer& Cmd() const { return m_Inner.Cmd(); }

        // This frame's view. Asserts the graph's user pointer is non-null, then
        // reinterprets it — SceneRenderer sets it to &view on every Execute.
        [[nodiscard]] const SceneView& View() const
        {
            void* userData = m_Inner.UserData();
            VE_ASSERT(userData != nullptr,
                      "ScenePassContext::View: the RenderGraph user pointer is null — "
                      "a ScenePass was replayed outside SceneRenderer::Execute");
            return *static_cast<const SceneView*>(userData);
        }

        [[nodiscard]] ImageView& Resolved(ResourceId id) const { return m_Inner.Resolved(id); }

    private:
        // A ScenePass wraps the RenderGraph PassContext it receives in its Declare
        // callback through ScenePass::Wrap; SceneRenderer guarantees the user pointer
        // is set on every Execute.
        friend class ScenePass;
        explicit ScenePassContext(PassContext& inner) : m_Inner(inner) {}

        PassContext& m_Inner;
    };

    // The wiring a SceneRenderer hands each pass: a named-slot struct, not a flat
    // in/out pair. The renderer fills the slots a given pass reads/writes; resources
    // flow both directions (a pass may produce one a later pass consumes), and a
    // pass may declare multiple RenderGraph passes. The forward case is degenerate:
    // one imported output id.
    struct PassIO
    {
        // The imported output id the terminal pass writes.
        ResourceId Output;
    };

    // A reusable, self-contained pipeline stage. The renderer calls Configure/Resize
    // when those knobs change, then Declare to contribute the pass's RenderGraph
    // pass(es) + record callback into the renderer's single internal graph.
    class ScenePass
    {
    public:
        virtual ~ScenePass() = default;

        virtual void Configure(const SceneRendererSettings&) {}
        virtual void Resize(uvec2) {}
        virtual void Declare(RenderGraph& graph, const PassIO& io) = 0;

    protected:
        // Type a RenderGraph record-time context as a ScenePassContext. A subclass
        // calls this inside its Declare record callback to read the per-frame
        // SceneView.
        [[nodiscard]] static ScenePassContext Wrap(PassContext& inner)
        {
            return ScenePassContext(inner);
        }
    };
}
