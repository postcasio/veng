#include <Veng/Gui/DocumentTexture.h>

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
                                         const uvec2 resolution, const f32 delta)
    {
        VE_ASSERT(resolution.x > 0 && resolution.y > 0,
                  "DocumentTexture::RenderToTarget: resolution must be positive (got {}x{})",
                  resolution.x, resolution.y);

        m_RenderedLastDrive = false;

        // Allocate or resize the HDR target to the requested resolution. The pass records at the
        // target's extent, so it needs no resize of its own.
        bool resolutionChanged = false;
        if (!m_Target)
        {
            m_Target = RenderTarget::Create({
                .Context = context,
                .Extent = resolution,
                .Name = "GuiSurface Target",
            });
            m_TargetExtent = resolution;
            resolutionChanged = true;
        }
        else if (m_TargetExtent != resolution)
        {
            m_Target->Resize(resolution);
            m_TargetExtent = resolution;
            resolutionChanged = true;
        }
        if (!m_Pass)
        {
            m_Pass = Renderer::GuiScenePass::Create({
                .Context = context,
                .Assets = assets,
                .Extent = resolution,
                .OutputFormat = RenderTarget::ColorFormat,
            });
        }

        // Dirty-gate: re-render only when the layout changed, a transition is animating, or the
        // resolution moved. A static document keeps its persistent target content and re-records
        // nothing. The caller refreshes the document's data bindings ahead of this call, so a moved
        // binding has already dirtied the layout and is reflected below.
        const bool needsRender =
            !m_EverRendered || resolutionChanged || document.IsDirty() || document.IsAnimating();
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
