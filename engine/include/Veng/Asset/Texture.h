#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <span>

// Texture: an Image + ImageView + Sampler, sampled bindlessly (set 0) via
// GetHandle()/GetSamplerHandle() once Finalize() has registered it.
namespace Veng
{
    class TaskSystem;

    template <typename T>
    class Task;
}

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

    // Texture: an Image + ImageView + Sampler. Creation is the worker-legal half
    // (image/view/sampler create + upload); registration into the bindless
    // registry is the main-thread Finalize() half — the two are split so the
    // async asset path can run creation on a worker and Finalize() on the
    // render-thread continuation. GetHandle()/GetSamplerHandle() are valid only
    // after Finalize().
    class Texture
    {
    public:
        // Synchronous create + blocking UploadSync. Unregistered; the caller
        // (the loader / AssetManager) calls Finalize() on the main thread.
        static Ref<Texture> Create(Renderer::Context& context, const TextureInfo& info);

        // Worker-legal create + async upload recorded on the transfer queue.
        // Returns the unregistered texture and a Task that completes once the
        // upload is submitted; the caller waits/registers on the main thread.
        static Ref<Texture> CreateAsync(Renderer::Context& context, const TextureInfo& info,
                                        TaskSystem& tasks, Task<void>& outUpload);

        ~Texture();

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        // Register the view + sampler into the bindless registry (set 0). Runs on
        // the main thread; idempotent guard via VE_ASSERT against double-register.
        void Finalize();

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
        bool m_Registered = false;
    };

    template <>
    struct AssetTypeTrait<Texture>
    {
        static constexpr AssetType Type = AssetType::Texture;
    };
}
