#pragma once

#include <Veng/Veng.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Renderer/BindlessRegistry.h>

namespace Veng
{
    class AssetManager;

    namespace Gui
    {
        class Document;
        class RenderTarget;
    }

    namespace Renderer
    {
        class Context;
        class CommandBuffer;
        class GuiScenePass;
    }
}

namespace Veng::Gui
{
    /// @brief Renders a document into an owned HDR render target, dirty-gated, for a material to sample.
    ///
    /// The world-space presenter of a Gui::DocumentHost: it owns a persistent HDR (RGBA16Sfloat)
    /// Gui::RenderTarget and the GuiScenePass that records a live document into it, sized to a
    /// requested resolution and re-allocated when the resolution changes. Each RenderToTarget lays
    /// the document out at the resolution and records it — but only when the document is dirty,
    /// animating, or the resolution moved (a static document keeps its persistent target content and
    /// records nothing). It exposes the rendered texture's bindless handle (GetOutputHandle) for a
    /// downstream material to sample; it knows nothing about materials, meshes, or domains.
    ///
    /// The GPU resources are materialized on the first RenderToTarget. Single-owner; the target and
    /// pass are released at destruction.
    class DocumentTexture
    {
    public:
        /// @brief Default-constructs an unmaterialized texture (its target and pass are empty).
        DocumentTexture();

        /// @brief Releases the owned render target and GuiScenePass.
        ~DocumentTexture();

        DocumentTexture(const DocumentTexture&) = delete;
        DocumentTexture& operator=(const DocumentTexture&) = delete;

        /// @brief Renders @p document into the owned HDR target, dirty-gated, leaving it shader-readable.
        ///
        /// Materializes the target and pass on first use (and re-allocates the target when
        /// @p resolution changes), then — when the document is dirty, animating, or the resolution
        /// changed — lays it out at @p resolution, records it into the HDR target through the pass,
        /// and transitions the target to a sampleable layout. Records into @p cmd ahead of the pass
        /// that samples the target, so the producer-before-consumer handoff needs no extra barrier.
        /// The data-binding refresh is the caller's, ahead of this call (see DocumentHost::Drive).
        /// @param context     The render context the target and pass allocate on.
        /// @param assets      The asset manager the GuiScenePass loads its gui shaders through.
        /// @param cmd         The command buffer the document render records into.
        /// @param document    The live document to record into the target.
        /// @param resolution  The HDR target size, in pixels, and the extent the document lays out at.
        /// @param delta       The frame time step, in seconds, forwarded to the document drive.
        /// @return True when this call re-recorded the document, false when the dirty-gate skipped it.
        /// @pre Both components of @p resolution are positive.
        bool RenderToTarget(Renderer::Context& context, AssetManager& assets,
                            Renderer::CommandBuffer& cmd, Document& document, uvec2 resolution,
                            f32 delta);

        /// @brief Returns the rendered texture's bindless handle, for a downstream sampler.
        ///
        /// The handle a material binds with Renderer::MaterialInstance::SetTextureHandle. Stable
        /// across a dirty-gate-skipped RenderToTarget (the target is not reallocated); invalidated by
        /// a resolution change — re-fetch after.
        [[nodiscard]] Renderer::TextureHandle GetOutputHandle() const;

        /// @brief Whether the most recent RenderToTarget re-recorded the document (the gate outcome).
        [[nodiscard]] bool WasRenderedLastDrive() const { return m_RenderedLastDrive; }

        /// @brief Returns the owned HDR render target, or nullptr before the first RenderToTarget.
        [[nodiscard]] RenderTarget* GetTarget() const { return m_Target.get(); }

    private:
        /// @brief The persistent HDR target the document records into and a material samples.
        Unique<RenderTarget> m_Target;
        /// @brief The owned pass recording the document into the target; per-texture so its per-frame
        ///        geometry ring is never shared with another texture's record in the same frame.
        Unique<Renderer::GuiScenePass> m_Pass;
        /// @brief The reusable draw-list buffer the document builds into each render.
        DrawList m_Draws;
        /// @brief The resolution the target was last sized to; a change re-sizes and re-renders.
        uvec2 m_TargetExtent{0, 0};
        /// @brief Seconds of drive accumulated from the per-frame delta, pushed as the pass's clock.
        f32 m_Time = 0.0f;
        /// @brief Whether any document render has happened yet (the first is unconditional).
        bool m_EverRendered = false;
        /// @brief Whether the most recent RenderToTarget re-recorded the document.
        bool m_RenderedLastDrive = false;
    };
}
