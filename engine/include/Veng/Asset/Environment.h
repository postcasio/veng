#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Renderer/BindlessRegistry.h>
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
    class Sampler;
}

namespace Veng
{
    /// @brief Construction parameters for an EnvironmentMap.
    struct EnvironmentMapData
    {
        /// @brief Debug name for the environment.
        string Name;
        /// @brief Panorama dimensions in pixels.
        uvec2 Extent;
        /// @brief Pixel format (an HDR float format).
        Renderer::Format Format;
        /// @brief Source equirectangular HDR pixel data.
        std::span<const u8> Pixels;
    };

    /// @brief An equirectangular HDR panorama, sampled bindlessly (set 0) for IBL generation.
    ///
    /// The panorama image is uploaded and registered into the bindless registry so the
    /// SceneRenderer's IBL-generation compute can sample it by handle to build the cubemap,
    /// irradiance, and prefiltered-specular maps. Like Texture, creation/upload are
    /// worker-legal and bindless registration is deferred to the main-thread Finalize().
    /// GetHandle()/GetSamplerHandle() are valid only after Finalize().
    class EnvironmentMap
    {
    public:
        ~EnvironmentMap();

        EnvironmentMap(const EnvironmentMap&) = delete;
        EnvironmentMap& operator=(const EnvironmentMap&) = delete;

        /// @brief Returns the environment's debug name.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Returns the underlying panorama Image.
        [[nodiscard]] const Ref<Renderer::Image>& GetImage() const { return m_Image; }

        /// @brief Returns the panorama ImageView used for sampling.
        [[nodiscard]] const Ref<Renderer::ImageView>& GetView() const { return m_View; }

        /// @brief Returns the panorama Sampler.
        [[nodiscard]] const Ref<Renderer::Sampler>& GetSampler() const { return m_Sampler; }

        /// @brief Returns the panorama dimensions in pixels.
        [[nodiscard]] uvec2 GetExtent() const { return m_Extent; }

        /// @brief Returns the bindless texture handle for the panorama (valid after Finalize()).
        [[nodiscard]] Renderer::TextureHandle GetHandle() const { return m_TextureHandle; }

        /// @brief Returns the bindless sampler handle for the panorama (valid after Finalize()).
        [[nodiscard]] Renderer::SamplerHandle GetSamplerHandle() const { return m_SamplerHandle; }

    private:
        friend class EnvironmentLoader;

        /// @brief Prepares an EnvironmentMap with a blocking upload, leaving it unregistered.
        /// @param context Render context the panorama image/view/sampler are created on.
        /// @param data    EnvironmentMap description (extent, format, pixels).
        /// @return The unregistered environment.
        static Ref<EnvironmentMap> PrepareSync(Renderer::Context& context,
                                               const EnvironmentMapData& data);

        /// @brief Prepares an EnvironmentMap with an async transfer-queue upload, leaving it unregistered.
        /// @param context    Render context the panorama image/view/sampler are created on.
        /// @param data       EnvironmentMap description (extent, format, pixels).
        /// @param tasks      Task system the async upload is recorded through.
        /// @param outUpload  Receives the upload task to wait on before Finalize().
        /// @return The unregistered environment.
        static Ref<EnvironmentMap> PrepareAsync(Renderer::Context& context,
                                                const EnvironmentMapData& data, TaskSystem& tasks,
                                                Task<void>& outUpload);

        /// @brief Registers the panorama view and sampler into the bindless registry (set 0).
        ///
        /// Runs on the render thread. Asserts against double-registration.
        void Finalize();

        EnvironmentMap(Renderer::Context& context, const EnvironmentMapData& data);

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

    /// @brief AssetTypeTrait specialization mapping EnvironmentMap to AssetType::Environment.
    template <>
    struct AssetTypeTrait<EnvironmentMap>
    {
        /// @brief The asset type tag for EnvironmentMap.
        static constexpr AssetType Type = AssetType::Environment;
    };
}
