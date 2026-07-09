#include <Veng/Gui/Surface.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/RenderTarget.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>

#include "../Renderer/Passes/GuiScenePass.h"

namespace Veng
{
    /// @brief The GPU state and live document a GuiSurface materializes on its first Drive.
    struct GuiSurfaceRuntime
    {
        /// @brief The live document driven each frame; instantiated from a recipe or injected.
        Unique<Gui::Document> Instance;
        /// @brief The persistent HDR target the document records into and the material samples.
        Unique<Gui::RenderTarget> Target;
        /// @brief The owned pass recording the document into the target; per-surface so its per-frame
        ///        geometry ring is never shared with another surface's record in the same frame.
        Unique<Renderer::GuiScenePass> Pass;
        /// @brief The reusable draw-list buffer the document builds into each render.
        Gui::DrawList Draws;
        /// @brief The resolution the target was last sized to; a change re-sizes and re-renders.
        uvec2 TargetExtent{0, 0};
        /// @brief Whether any document render has happened yet (the first is unconditional).
        bool EverRendered = false;
        /// @brief Whether the most recent Drive re-recorded the document.
        bool RenderedLastDrive = false;
        /// @brief Whether the emissive material's white default has been applied once.
        bool EmissiveSeeded = false;
    };

    namespace
    {
        /// @brief Whether a material declares a field of the given name and kind.
        bool HasField(const MaterialInstance& material, std::string_view name,
                      MaterialField::FieldKind kind)
        {
            for (const MaterialField& field : material.GetFields())
            {
                if (field.Name == name && field.Kind == kind)
                {
                    return true;
                }
            }
            return false;
        }

        /// @brief Binds the document handle onto the surface material for the chosen domain.
        void BindMaterial(GuiSurfaceRuntime& runtime, GuiSurfaceDomain domain,
                          MaterialInstance& material, Renderer::TextureHandle handle,
                          Renderer::SamplerHandle sampler)
        {
            if (domain == GuiSurfaceDomain::OpaqueEmissive)
            {
                if (HasField(material, "EmissiveTexture", MaterialField::FieldKind::TextureHandle))
                {
                    material.SetTextureHandle("EmissiveTexture", handle);
                }
                if (HasField(material, "EmissiveSampler", MaterialField::FieldKind::SamplerHandle))
                {
                    material.SetSamplerHandle("EmissiveSampler", sampler);
                }
                // The emissive pass computes EmissiveColor * texel; a white default lets the
                // document value pass through unmodulated. Seeded once so a game tint set later
                // wins over the default.
                if (!runtime.EmissiveSeeded &&
                    HasField(material, "EmissiveColor", MaterialField::FieldKind::Param))
                {
                    material.SetParam("EmissiveColor", vec4(1.0f, 1.0f, 1.0f, 0.0f));
                    runtime.EmissiveSeeded = true;
                }
                return;
            }

            // Translucent: the panel material samples a Texture/Sampler pair as its radiance.
            if (HasField(material, "Texture", MaterialField::FieldKind::TextureHandle))
            {
                material.SetTextureHandle("Texture", handle);
            }
            if (HasField(material, "Sampler", MaterialField::FieldKind::SamplerHandle))
            {
                material.SetSamplerHandle("Sampler", sampler);
            }
        }
    }

    GuiSurface::GuiSurface() = default;
    GuiSurface::~GuiSurface() = default;
    GuiSurface::GuiSurface(GuiSurface&&) noexcept = default;
    GuiSurface& GuiSurface::operator=(GuiSurface&&) noexcept = default;

    void GuiSurface::SetDocument(Unique<Gui::Document> document)
    {
        if (!Runtime)
        {
            Runtime = CreateUnique<GuiSurfaceRuntime>();
        }
        Runtime->Instance = std::move(document);
    }

    Gui::Document* GuiSurface::GetDocument() const
    {
        return Runtime ? Runtime->Instance.get() : nullptr;
    }

    Gui::RenderTarget* GuiSurface::GetTarget() const
    {
        return Runtime ? Runtime->Target.get() : nullptr;
    }

    bool GuiSurface::WasRenderedLastDrive() const
    {
        return Runtime && Runtime->RenderedLastDrive;
    }

    bool GuiSurface::Drive(Renderer::Context& context, AssetManager& assets,
                           Renderer::CommandBuffer& cmd, Renderer::SamplerHandle sampler,
                           MaterialInstance* material, f32 delta) const
    {
        VE_ASSERT(Resolution.x > 0 && Resolution.y > 0,
                  "GuiSurface::Drive: Resolution must be positive (got {}x{})", Resolution.x,
                  Resolution.y);

        if (!Runtime)
        {
            Runtime = CreateUnique<GuiSurfaceRuntime>();
        }
        GuiSurfaceRuntime& runtime = *Runtime;
        runtime.RenderedLastDrive = false;

        // Instantiate a cooked recipe on first use; an imperative document is injected through
        // SetDocument. With neither available yet (a recipe still loading), there is nothing to draw.
        if (!runtime.Instance)
        {
            if (Document.IsLoaded())
            {
                runtime.Instance = Gui::Document::Instantiate(*Document.Get(), assets);
            }
            else
            {
                return false;
            }
        }

        // Allocate or resize the HDR target to the requested resolution. The pass records at the
        // target's extent, so it needs no resize of its own.
        bool resolutionChanged = false;
        if (!runtime.Target)
        {
            runtime.Target = Gui::RenderTarget::Create({
                .Context = context,
                .Extent = Resolution,
                .Name = "GuiSurface Target",
            });
            runtime.TargetExtent = Resolution;
            resolutionChanged = true;
        }
        else if (runtime.TargetExtent != Resolution)
        {
            runtime.Target->Resize(Resolution);
            runtime.TargetExtent = Resolution;
            resolutionChanged = true;
        }
        if (!runtime.Pass)
        {
            runtime.Pass = Renderer::GuiScenePass::Create({
                .Context = context,
                .Assets = assets,
                .Extent = Resolution,
                .OutputFormat = Gui::RenderTarget::ColorFormat,
            });
        }

        Gui::Document& document = *runtime.Instance;

        // The data-binding refresh precedes the dirty check: a moved binding dirties the layout, so
        // it is reflected in the gate below (no per-frame game code for a data-bound panel).
        document.UpdateBindings();

        // Dirty-gate: re-render only when the layout changed, a transition is animating, or the
        // resolution moved. A static panel keeps its persistent target content and re-records nothing.
        const bool needsRender = !runtime.EverRendered || resolutionChanged || document.IsDirty() ||
                                 document.IsAnimating();
        if (needsRender)
        {
            const vec2 available(static_cast<f32>(Resolution.x), static_cast<f32>(Resolution.y));
            runtime.Draws.Clear();
            document.Drive(available, delta, runtime.Draws);
            runtime.Pass->SetDrawList(runtime.Draws);
            runtime.Pass->RenderToTarget(cmd, *runtime.Target);
            runtime.EverRendered = true;
            runtime.RenderedLastDrive = true;
        }

        // Bind the target handle onto the surface material every frame: SetTextureHandle writes the
        // current frame-in-flight region, so the handle must land regardless of the dirty-gate.
        if (material != nullptr)
        {
            BindMaterial(runtime, Domain, *material, runtime.Target->GetOutputHandle(), sampler);
        }

        return runtime.RenderedLastDrive;
    }
}
