#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetBuild.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <span>

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
    /// @brief Construction parameters for a Texture.
    struct TextureData
    {
        /// @brief Debug name for the texture.
        string Name;
        /// @brief Image dimensions in pixels.
        uvec2 Extent;
        /// @brief Pixel format.
        Renderer::Format Format;
        /// @brief Source pixel data.
        std::span<const u8> Pixels;
        /// @brief Sampler parameters.
        Renderer::SamplerInfo Sampler;
    };

    /// @brief An Image + ImageView + Sampler, sampled bindlessly (set 0) after Finalize() registers it.
    ///
    /// Creation (image/view/sampler create + upload) is worker-legal; registration into the
    /// bindless registry is deferred to the main-thread Finalize() step so the async asset
    /// path can run creation on a worker and finalize on the render-thread continuation.
    /// GetHandle()/GetSamplerHandle() are valid only after Finalize().
    class Texture
    {
    public:
        ~Texture();

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        /// @brief Returns the texture's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the underlying Image.
        [[nodiscard]] const Ref<Renderer::Image>& GetImage() const { return m_Image; }

        /// @brief Returns the ImageView used for sampling.
        [[nodiscard]] const Ref<Renderer::ImageView>& GetView() const { return m_View; }

        /// @brief Returns the Sampler.
        [[nodiscard]] const Ref<Renderer::Sampler>& GetSampler() const { return m_Sampler; }

        /// @brief Returns the texture's pixel format.
        [[nodiscard]] Renderer::Format GetFormat() const { return m_Format; }

        /// @brief Returns the texture's dimensions in pixels.
        [[nodiscard]] uvec2 GetExtent() const { return m_Extent; }

        /// @brief Returns the bindless texture handle (valid after Finalize()).
        [[nodiscard]] Renderer::TextureHandle GetHandle() const { return m_TextureHandle; }

        /// @brief Returns the bindless sampler handle (valid after Finalize()).
        [[nodiscard]] Renderer::SamplerHandle GetSamplerHandle() const { return m_SamplerHandle; }

    private:
        friend class TextureLoader;
        friend Task<Detail::BuiltAsset<Texture>>
        Detail::SubmitAssetBuild(Renderer::Context& context, TaskSystem& tasks, TextureData data);
        friend Ref<Texture> Detail::BuildAssetSync(Renderer::Context& context,
                                                   const TextureData& data);

        /// @brief Prepares a Texture with a blocking upload, leaving it unregistered.
        ///
        /// Constructs the image/view/sampler and uploads the pixels through the blocking
        /// UploadSync path. The result must be Finalize()d on the render thread before sampling.
        /// @param context Render context the image/view/sampler are created on.
        /// @param data    Texture description (extent, format, pixels, sampler settings).
        /// @return The unregistered texture.
        static Ref<Texture> PrepareSync(Renderer::Context& context, const TextureData& data);

        /// @brief Prepares a Texture with an async transfer-queue upload, leaving it unregistered.
        ///
        /// Constructs the image/view/sampler and records the upload on the transfer queue,
        /// returning the unregistered texture and a Task that completes once the upload is
        /// submitted. The result must be Finalize()d on the render thread before sampling.
        /// @param context    Render context the image/view/sampler are created on.
        /// @param data       Texture description (extent, format, pixels, sampler settings).
        /// @param tasks      Task system the async upload is recorded through.
        /// @param outUpload  Receives the upload task to wait on before Finalize().
        /// @return The unregistered texture.
        static Ref<Texture> PrepareAsync(Renderer::Context& context, const TextureData& data,
                                         TaskSystem& tasks, Task<void>& outUpload);

        /// @brief Registers the view and sampler into the bindless registry (set 0).
        ///
        /// Runs on the render thread. Asserts against double-registration.
        void Finalize();

        Texture(Renderer::Context& context, const TextureData& data);

        /// @brief Back-reference for deferred destruction; resource must not outlive its context.
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

    /// @brief AssetTypeTrait specialization mapping Texture to AssetType::Texture.
    template <>
    struct AssetTypeTrait<Texture>
    {
        /// @brief The asset type tag for Texture.
        static constexpr AssetType Type = AssetType::Texture;
    };
}
