#include <Veng/Gui/DocumentTexture.h>

#include <algorithm>
#include <cmath>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/RenderTarget.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>

#include "../Renderer/Passes/GuiScenePass.h"

namespace Veng::Gui
{
    DocumentTexture::DocumentTexture() = default;
    DocumentTexture::~DocumentTexture() = default;

    bool DocumentTexture::RenderToTarget(Renderer::Context& context, AssetManager& assets,
                                         Renderer::CommandBuffer& cmd, Document& document,
                                         const uvec2 resolution, const f32 scale, const f32 delta)
    {
        VE_ASSERT(resolution.x > 0 && resolution.y > 0,
                  "DocumentTexture::RenderToTarget: resolution must be positive (got {}x{})",
                  resolution.x, resolution.y);
        VE_ASSERT(scale > 0.0f, "DocumentTexture::RenderToTarget: scale must be positive (got {})",
                  scale);

        m_RenderedLastDrive = false;

        // The target is physical pixels, the layout extent is logical points: one scale factor apart.
        // Rounded, then floored at one pixel per axis so a small resolution and a small scale cannot
        // multiply out to a zero extent the target creation would abort on.
        const uvec2 targetExtent(
            std::max(1u, static_cast<u32>(std::lround(static_cast<f32>(resolution.x) * scale))),
            std::max(1u, static_cast<u32>(std::lround(static_cast<f32>(resolution.y) * scale))));

        // Allocate or resize the HDR target. The pass records at the target's extent, so it needs no
        // resize of its own; the scale is gated on beside the extent because it changes the recorded
        // magnification even when the extent happens not to move.
        bool sizingChanged = m_Scale != scale;
        if (!m_Target)
        {
            m_Target = RenderTarget::Create({
                .Context = context,
                .Extent = targetExtent,
                .Name = "GuiSurface Target",
            });
            m_TargetExtent = targetExtent;
            sizingChanged = true;
        }
        else if (m_TargetExtent != targetExtent)
        {
            m_Target->Resize(targetExtent);
            m_TargetExtent = targetExtent;
            sizingChanged = true;
        }
        if (!m_Pass)
        {
            m_Pass = Renderer::GuiScenePass::Create({
                .Context = context,
                .Assets = assets,
                .Extent = targetExtent,
                .OutputFormat = RenderTarget::ColorFormat,
            });
        }

        m_Time += delta;
        m_Pass->SetTime(m_Time);
        // The draw list is logical points; this is what magnifies it onto the physical target.
        m_Pass->SetUiScale(scale);
        m_Scale = scale;

        // A material fill's animation rides the pass's clock, so its pixels change with no dirty
        // signal from the tree at all — a document carrying one is re-recorded every drive.
        const bool hasMaterialFill =
            std::ranges::any_of(m_Draws.GetRuns(), [](const DrawRun& run)
                                { return run.Pipeline == GuiPipeline::Material; });

        // Dirty-gate: re-render only when the layout changed, a transition is animating, or the
        // target sizing moved. A static document keeps its persistent target content and re-records
        // nothing. The caller refreshes the document's data bindings ahead of this call, so a moved
        // binding has already dirtied the layout and is reflected below.
        const bool needsRender = !m_EverRendered || sizingChanged || document.IsDirty() ||
                                 document.IsAnimating() || hasMaterialFill;
        if (needsRender)
        {
            const vec2 available(static_cast<f32>(resolution.x), static_cast<f32>(resolution.y));
            m_Draws.Clear();
            document.Drive(available, delta, m_Draws);
            m_Pass->SetDrawList(m_Draws);
            m_Pass->RenderToTarget(cmd, *m_Target);
            m_EverRendered = true;
            m_RenderedLastDrive = true;
        }

        return m_RenderedLastDrive;
    }

    Renderer::TextureHandle DocumentTexture::GetOutputHandle() const
    {
        VE_ASSERT(m_Target != nullptr,
                  "DocumentTexture::GetOutputHandle: no target — call RenderToTarget first");
        return m_Target->GetOutputHandle();
    }
}
