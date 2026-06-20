#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief Construction parameters for a Sampler.
    struct SamplerInfo
    {
        /// @brief Debug name.
        string Name;
        /// @brief Magnification filter.
        Filter MagFilter = Filter::Linear;
        /// @brief Minification filter.
        Filter MinFilter = Filter::Linear;
        /// @brief Mip-level interpolation mode.
        MipmapMode MipmapMode = MipmapMode::Linear;
        /// @brief U-axis wrap mode.
        AddressMode AddressModeU = AddressMode::Repeat;
        /// @brief V-axis wrap mode.
        AddressMode AddressModeV = AddressMode::Repeat;
        /// @brief W-axis wrap mode.
        AddressMode AddressModeW = AddressMode::Repeat;
        /// @brief Bias added to the computed mip LOD.
        f32 MipLodBias = 0;
        /// @brief Whether anisotropic filtering is active.
        bool AnisotropyEnabled = true;
        /// @brief Maximum anisotropy samples (1–device maximum).
        f32 MaxAnisotropy = 8;
        /// @brief Whether depth-comparison sampling is active.
        bool CompareEnable = false;
        /// @brief Comparison operator used when CompareEnable is true.
        CompareOp CompareOp = CompareOp::Always;
        /// @brief Minimum clamp for the computed mip LOD.
        f32 MinLod = 0;
        /// @brief Maximum clamp for the computed mip LOD.
        f32 MaxLod = 1;
        /// @brief Border color for ClampToBorder address mode.
        BorderColor BorderColor = BorderColor::OpaqueBlack;
        /// @brief Whether texture coordinates are in texel space rather than [0,1].
        bool UnnormalizedCoordinates = false;
    };

    /// @brief A GPU sampler object controlling how images are filtered and addressed.
    ///
    /// Created via Sampler::Create; shared across any pipeline or descriptor set that
    /// needs the same sampling parameters.
    class Sampler
    {
    public:
        /// @brief Creates a sampler with the given parameters.
        /// @return A shared reference to the new sampler.
        static Ref<Sampler> Create(Context& context, const SamplerInfo& info)
        {
            return Ref<Sampler>(new Sampler(context, info));
        }

        /// @brief Defers destruction of the underlying Vulkan sampler until the GPU is done with it.
        ~Sampler();

        Sampler(const Sampler&) = delete;
        Sampler& operator=(const Sampler&) = delete;

        /// @brief Returns the debug name supplied at creation.
        [[nodiscard]] const string& GetName() const { return m_Name; }

        /// @brief Opaque backend handle; defined in Sampler.cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        Sampler(Context& context, const SamplerInfo& info);

        /// @brief Context this resource was created with; must outlive the sampler.
        Context& m_Context;
        /// @brief Debug name.
        string m_Name;
        /// @brief Backend Vulkan sampler.
        Unique<Native> m_Native;
    };
}
