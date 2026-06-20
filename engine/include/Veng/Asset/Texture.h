#pragma once

#include <Veng/Veng.h>
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
    struct TextureInfo
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
        /// @brief Synchronous create + blocking UploadSync. Unregistered; the caller must call Finalize() on the main thread.
        static Ref<Texture> Create(Renderer::Context& context, const TextureInfo& info);

        /// @brief Worker-legal create + async upload recorded on the transfer queue.
        ///
        /// Returns the unregistered texture and a Task that completes once the upload is submitted.
        /// The caller waits for the task and calls Finalize() on the main thread.
        /// @param context    Render context the image/view/sampler are created on.
        /// @param info       Texture description (extent, format, pixels, sampler settings).
        /// @param tasks      Task system the async upload is recorded through.
        /// @param outUpload  Receives the upload task to wait on before calling Finalize().
        static Ref<Texture> CreateAsync(Renderer::Context& context, const TextureInfo& info,
                                        TaskSystem& tasks, Task<void>& outUpload);

        ~Texture();

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        /// @brief Registers the view and sampler into the bindless registry (set 0).
        ///
        /// Runs on the main thread. Asserts against double-registration.
        void Finalize();

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
        Texture(Renderer::Context& context, const TextureInfo& info);

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
