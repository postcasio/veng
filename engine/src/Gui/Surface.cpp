#include <Veng/Gui/Surface.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentHost.h>
#include <Veng/Gui/DocumentTexture.h>
#include <Veng/Gui/UIDocument.h>

namespace Veng
{
    /// @brief The document core and its render-to-texture presenter a GuiSurface materializes on Drive.
    struct GuiSurfaceRuntime
    {
        /// @brief The live/bound document core; constructed on the first Drive (needs the asset manager).
        Unique<Gui::DocumentHost> Host;
        /// @brief The render-to-texture presenter: the owned HDR target and the pass that fills it.
        Gui::DocumentTexture Texture;
        /// @brief A document injected through SetDocument before the host exists, adopted on first Drive.
        Unique<Gui::Document> Pending;
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
        // The host is built lazily on the first Drive (it needs the asset manager), so before then an
        // injected document waits in Pending; after, it is adopted straight into the host.
        if (Runtime->Host)
        {
            Runtime->Host->SetDocument(std::move(document));
        }
        else
        {
            Runtime->Pending = std::move(document);
        }
    }

    Gui::Document* GuiSurface::GetDocument() const
    {
        if (!Runtime)
        {
            return nullptr;
        }
        return Runtime->Host ? Runtime->Host->Get() : Runtime->Pending.get();
    }

    Gui::RenderTarget* GuiSurface::GetTarget() const
    {
        return Runtime ? Runtime->Texture.GetTarget() : nullptr;
    }

    bool GuiSurface::WasRenderedLastDrive() const
    {
        return Runtime && Runtime->Texture.WasRenderedLastDrive();
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

        // Build the document core on first use, adopting any document injected before the host existed.
        if (!runtime.Host)
        {
            runtime.Host = CreateUnique<Gui::DocumentHost>(assets, assets.GetTypeRegistry());
            if (runtime.Pending)
            {
                runtime.Host->SetDocument(std::move(runtime.Pending));
            }
        }

        // Instantiate a cooked recipe on first use; an imperative document is injected through
        // SetDocument. With neither available yet (a recipe still loading), there is nothing to draw.
        if (runtime.Host->Get() == nullptr)
        {
            if (Document.IsLoaded())
            {
                runtime.Host->SetDocument(Gui::Document::Instantiate(*Document.Get(), assets));
            }
            else
            {
                return false;
            }
        }

        // Drive the core (refresh bindings, expose the live tree), then render it into the HDR target
        // dirty-gated. UpdateBindings precedes the texture's dirty check, so a moved binding re-renders.
        Gui::Document* const document = runtime.Host->Drive();
        if (document == nullptr)
        {
            return false;
        }
        const bool rendered =
            runtime.Texture.RenderToTarget(context, assets, cmd, *document, Resolution, delta);

        // Bind the target handle onto the surface material every frame: SetTextureHandle writes the
        // current frame-in-flight region, so the handle must land regardless of the dirty-gate.
        if (material != nullptr)
        {
            BindMaterial(runtime, Domain, *material, runtime.Texture.GetOutputHandle(), sampler);
        }

        return rendered;
    }
}
