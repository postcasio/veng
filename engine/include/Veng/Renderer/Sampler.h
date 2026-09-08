#pragma once

#include <algorithm>

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    class Context;

    /// @brief The SamplerInfo::MaxLod value that applies no upper clamp of its own, leaving the
    /// image view's level count as the only bound on the mip a sample reads.
    ///
    /// The backend clamps the selected level into the view's range regardless, so any value at or
    /// above the deepest level a view can carry behaves identically; this is the one to write when
    /// the whole chain is wanted, because it does not vary with the image and so keeps otherwise
    /// identical descriptions identical.
    inline constexpr f32 LodClampNone = 1000.0f;

    /// @brief The engine's default anisotropy sample count for scene-texture sampling.
    ///
    /// The one value shared by SamplerInfo::MaxAnisotropy's default and the BindlessRegistry's
    /// default global anisotropy state, so a sampler built at the default global state is
    /// byte-identical to one built before the global control existed. The Vulkan spec guarantees a
    /// device maximum of at least 16 when the samplerAnisotropy feature is enabled, so this value
    /// is always representable.
    inline constexpr f32 DefaultMaxAnisotropy = 8.0f;

    /// @brief The anisotropy a sampler may actually use, resolved against the device limit.
    struct ResolvedAnisotropy
    {
        /// @brief Whether anisotropic filtering is active.
        bool Enabled = false;
        /// @brief The sample count, in [1, deviceMax] when enabled, else 1.
        f32 MaxAnisotropy = 1.0f;
    };

    /// @brief Resolves an anisotropy request against a device's maximum sample count.
    ///
    /// A disabled request resolves to filtering off at 1x regardless of the requested value; an
    /// enabled request is clamped into [1, deviceMax]. This is the one place a settable anisotropy
    /// is bounded, and it is pure so the clamp is unit-testable with no device.
    /// @param enabled    Whether anisotropic filtering was requested.
    /// @param requested  The requested sample count.
    /// @param deviceMax  The device's maxSamplerAnisotropy limit.
    /// @return The enabled flag and clamped sample count a sampler may be built with.
    [[nodiscard]] inline ResolvedAnisotropy ResolveAnisotropy(bool enabled, f32 requested,
                                                              f32 deviceMax)
    {
        if (!enabled)
        {
            return ResolvedAnisotropy{.Enabled = false, .MaxAnisotropy = 1.0f};
        }
        return ResolvedAnisotropy{.Enabled = true,
                                  .MaxAnisotropy = std::clamp(requested, 1.0f, deviceMax)};
    }

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
        f32 MaxAnisotropy = DefaultMaxAnisotropy;
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
        /// @brief Whether this sampler's anisotropy follows the engine's global filtering control.
        ///
        /// Set only by the scene-texture path (the cooked/runtime Texture asset). When true,
        /// BindlessRegistry::AcquireSampler builds the sampler with the current global anisotropy in
        /// place of the AnisotropyEnabled/MaxAnisotropy above, and a later SetGlobalAnisotropy
        /// rebuilds it live. It is part of the cache key, so an opted-in description never shares a
        /// slot with an otherwise-identical one that opted out — which is what confines the global
        /// override to scene textures and leaves the funnel's internal render-target samplers as
        /// authored.
        bool HonorGlobalAnisotropy = false;
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
        [[nodiscard]] const string& GetName() const { return m_Info.Name; }

        /// @brief Returns the description this sampler was actually created from.
        ///
        /// The resolved description — for a sampler the registry built under the global anisotropy
        /// control, the anisotropy fields are the resolved global values, not the caller's request.
        [[nodiscard]] const SamplerInfo& GetInfo() const { return m_Info; }

        /// @brief Opaque backend handle; defined in Sampler.cpp.
        struct Native;
        /// @brief Returns the backend handle. Mutable ref from a const method by design — see Native.h.
        [[nodiscard]] Native& GetNative() const;

    private:
        Sampler(Context& context, const SamplerInfo& info);

        /// @brief Context this resource was created with; must outlive the sampler.
        Context& m_Context;
        /// @brief The description the sampler was created from.
        SamplerInfo m_Info;
        /// @brief Backend Vulkan sampler.
        Unique<Native> m_Native;
    };
}
