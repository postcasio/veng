#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <span>

// Texture: an Image + ImageView + Sampler, uploaded once and registered into
// the bindless registry (set 0) so it can be sampled via
// GetHandle()/GetSamplerHandle().
namespace Veng::Renderer
{
    class Context;
    class Image;
    class ImageView;
}

namespace Veng
{
    struct TextureInfo
    {
        string Name;
        uvec2 Extent;
        Renderer::Format Format;
        std::span<const u8> Pixels;
        Renderer::SamplerInfo Sampler;
    };

    class Texture
    {
    public:
        static Ref<Texture> Create(Renderer::Context& context, const TextureInfo& info)
        {
            return Ref<Texture>(new Texture(context, info));
        }

        ~Texture();

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const Ref<Renderer::Image>& GetImage() const { return m_Image; }
        [[nodiscard]] const Ref<Renderer::ImageView>& GetView() const { return m_View; }
        [[nodiscard]] const Ref<Renderer::Sampler>& GetSampler() const { return m_Sampler; }
        [[nodiscard]] Renderer::Format GetFormat() const { return m_Format; }
        [[nodiscard]] uvec2 GetExtent() const { return m_Extent; }

        [[nodiscard]] Renderer::TextureHandle GetHandle() const { return m_TextureHandle; }
        [[nodiscard]] Renderer::SamplerHandle GetSamplerHandle() const { return m_SamplerHandle; }

    private:
        Texture(Renderer::Context& context, const TextureInfo& info);

        // The context this resource was created with (deferred-destruction
        // back-ref; a resource must not outlive its context).
        Renderer::Context& m_Context;
        string m_Name;
        uvec2 m_Extent;
        Renderer::Format m_Format;

        Ref<Renderer::Image> m_Image;
        Ref<Renderer::ImageView> m_View;
        Ref<Renderer::Sampler> m_Sampler;

        Renderer::TextureHandle m_TextureHandle;
        Renderer::SamplerHandle m_SamplerHandle;
    };

    template <>
    struct AssetTypeTrait<Texture>
    {
        static constexpr AssetType Type = AssetType::Texture;
    };
}
