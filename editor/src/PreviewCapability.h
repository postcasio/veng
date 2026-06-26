#pragma once

#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Project/CompressionFormat.h>
#include <Veng/Veng.h>

namespace Veng
{
    namespace Renderer
    {
        class Context;
    }
}

namespace VengEditor
{
    /// @brief Whether a build configuration's textures can be sampled on this GPU, with a reason.
    ///
    /// Carries the gate result so the UI can both grey out an incompatible configuration and
    /// state why ("this GPU lacks ASTC").
    struct PreviewCapability
    {
        /// @brief True when every format the configuration resolves to is samplable here.
        bool Previewable = false;
        /// @brief Empty when Previewable; otherwise the host-facing reason it is not.
        Veng::string Reason;
    };

    /// @brief Reports whether a single codec output can be sampled on the host GPU.
    ///
    /// Gates on the device *feature*, not a platform label: a BC format needs
    /// textureCompressionBC enabled, an ASTC format needs textureCompressionASTC_LDR, and an
    /// uncompressed format always samples. This is the one question the preview gate and the
    /// Project Settings capability warning both ask.
    /// @param format   The codec output to test.
    /// @param context  The render context whose enabled features decide eligibility.
    /// @return True when the host GPU can sample an image in this format.
    [[nodiscard]] bool IsFormatPreviewable(Veng::CompressionFormat format,
                                           const Veng::Renderer::Context& context);

    /// @brief Reports whether a build configuration is previewable on the host GPU.
    ///
    /// Intersects the configuration's resolved role formats with the host's enabled features
    /// (every role's format must be samplable). The result's Reason names the first
    /// host-incompatible codec so the selector can disable the entry with a stated cause.
    /// @param config   The configuration whose role table is tested.
    /// @param context  The render context whose enabled features decide eligibility.
    /// @return The gate result: previewable plus an empty reason, or not plus the cause.
    [[nodiscard]] PreviewCapability IsConfigPreviewable(const Veng::BuildConfiguration& config,
                                                        const Veng::Renderer::Context& context);

    /// @brief Builds a host-safe role table the editor previews through by default.
    ///
    /// Every role resolves to a format the host GPU is guaranteed to sample, so the editor never
    /// hands the GPU an unsamplable blob: uncompressed unorm/sRGB for the LDR roles and the
    /// uncompressed float for HDR. Independent of any ship configuration — the safe profile
    /// overrides the active config's formats for the editor's own cook.
    /// @return A RoleToFormat resolving every role to an uncompressed, always-samplable format.
    [[nodiscard]] Veng::RoleToFormat HostSafeFormats();

    /// @brief The host-safe build configuration the editor cooks previews against by default.
    ///
    /// Wraps HostSafeFormats() in a named configuration so the cook path threads it exactly like
    /// a ship configuration. The build of any real configuration stays unrestricted; only this
    /// editor-side preview cook is clamped.
    /// @return A configuration named "preview (host-safe)" over the uncompressed profile.
    [[nodiscard]] Veng::BuildConfiguration HostSafeConfiguration();
}
