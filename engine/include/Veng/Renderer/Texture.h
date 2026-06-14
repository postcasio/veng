#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <span>

// Texture (planset-5 plan 06): the first full asset-type vertical slice. An
// Image + ImageView + Sampler, uploaded once and registered into the bindless
// registry (set 0) so it can be sampled via GetHandle()/GetSamplerHandle().
namespace Veng::Renderer
{
    class Context;
    class Image;
    class ImageView;

    struct TextureInfo
    {
        string Name;
        uvec2 Extent;
        Format Format;
        std::span<const u8> Pixels;
        SamplerInfo Sampler;
    };

    class Texture
    {
    public:
        static Ref<Texture> Create(Context& context, const TextureInfo& info)
        {
            return Ref<Texture>(new Texture(context, info));
        }

        ~Texture();

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        [[nodiscard]] const string& GetName() const { return m_Name; }
        [[nodiscard]] const Ref<Image>& GetImage() const { return m_Image; }
        [[nodiscard]] const Ref<ImageView>& GetView() const { return m_View; }
        [[nodiscard]] const Ref<Sampler>& GetSampler() const { return m_Sampler; }
        [[nodiscard]] Format GetFormat() const { return m_Format; }
        [[nodiscard]] uvec2 GetExtent() const { return m_Extent; }

        [[nodiscard]] TextureHandle GetHandle() const { return m_TextureHandle; }
        [[nodiscard]] SamplerHandle GetSamplerHandle() const { return m_SamplerHandle; }

    private:
        Texture(Context& context, const TextureInfo& info);

        // The context this resource was created with (deferred-destruction
        // back-ref; a resource must not outlive its context).
        Context& m_Context;
        string m_Name;
        uvec2 m_Extent;
        Format m_Format;

        Ref<Image> m_Image;
        Ref<ImageView> m_View;
        Ref<Sampler> m_Sampler;

        TextureHandle m_TextureHandle;
        SamplerHandle m_SamplerHandle;
    };
}

namespace Veng
{
    template <>
    struct AssetTypeTrait<Renderer::Texture>
    {
        static constexpr AssetType Type = AssetType::Texture;
    };
}
