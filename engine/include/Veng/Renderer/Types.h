#pragma once

#include <Veng/Veng.h>
#include <variant>

/// @brief Engine-owned vocabulary types for the renderer surface.
///
/// Consumers write these instead of vk:: types; the backend maps them to Vulkan in
/// TypeMapping.h with exhaustive switches. Enumerators are populated on demand — the
/// backend mapping asserts on unmapped values, so a missing entry is a loud, one-line fix.
namespace Veng::Renderer
{
    /// @brief Pixel format of an image or attachment.
    enum class Format : u8
    {
        /// @brief No format / uninitialized.
        Undefined,
        /// @brief 8-bit single-channel, normalized [0,1].
        R8Unorm,
        /// @brief 8-bit per channel RGBA, normalized [0,1].
        RGBA8Unorm,
        /// @brief 8-bit per channel RGBA, sRGB-encoded.
        RGBA8Srgb,
        /// @brief 8-bit per channel BGRA, sRGB-encoded (common swapchain format).
        BGRA8Srgb,
        /// @brief 16-bit single-channel float.
        R16Sfloat,
        /// @brief 16-bit per channel RGBA float.
        RGBA16Sfloat,
        /// @brief 32-bit single-channel float.
        R32Sfloat,
        /// @brief 32-bit per channel RG float.
        RG32Sfloat,
        /// @brief 32-bit per channel RGB float.
        RGB32Sfloat,
        /// @brief 32-bit per channel RGBA float.
        RGBA32Sfloat,
        /// @brief 16-bit depth, normalized.
        D16Unorm,
        /// @brief 32-bit depth float.
        D32Sfloat,
        /// @brief 8-bit stencil.
        S8Uint,
        /// @brief 16-bit depth + 8-bit stencil.
        D16UnormS8Uint,
        /// @brief 24-bit depth + 8-bit stencil.
        D24UnormS8Uint,
        /// @brief 32-bit depth float + 8-bit stencil.
        D32SfloatS8Uint,
        /// @brief 24-bit depth packed in 32 bits (8 bits unused).
        X8D24UnormPack32,
        /// @brief 16-bit per channel RG float. Appended (not inserted) so the underlying
        ///        enum values cooked blobs persist by integer stay stable.
        RG16Sfloat,
        /// @brief 10-bit RGB + 2-bit alpha, normalized, packed in 32 bits (BGRA order).
        ///        The HDR10 swapchain format. Appended for cooked-blob integer stability.
        A2B10G10R10Unorm,
        /// @brief 16-bit per channel RGBA unsigned integer (read as uint4 in-shader).
        ///        The skinned-mesh bone-index attribute format. Appended for cooked-blob
        ///        integer stability.
        RGBA16Uint,
        /// @brief BC7 block-compressed RGBA, normalized [0,1], linear-encoded (4x4 16-byte blocks).
        ///        Appended at the fixed ordinal 21 for cooked-blob integer stability; the cooker
        ///        writes the literal and the texture loader reads it.
        BC7Unorm,
        /// @brief BC7 block-compressed RGBA, sRGB-encoded (4x4 16-byte blocks).
        ///        Appended at the fixed ordinal 22 for cooked-blob integer stability; the cooker
        ///        writes the literal and the texture loader reads it.
        BC7Srgb,
        /// @brief ASTC LDR block-compressed RGBA, normalized [0,1], linear-encoded (4x4 16-byte
        ///        blocks). Appended at the fixed ordinal 23 for cooked-blob integer stability; the
        ///        cooker writes the literal and the texture loader reads it.
        ASTC4x4Unorm,
        /// @brief ASTC LDR block-compressed RGBA, sRGB-encoded (4x4 16-byte blocks).
        ///        Appended at the fixed ordinal 24 for cooked-blob integer stability; the cooker
        ///        writes the literal and the texture loader reads it.
        ASTC4x4Srgb,
        /// @brief 32-bit single-channel unsigned integer (read as uint in-shader).
        ///        The entity-id picking target's color format. Appended (not inserted) so the
        ///        underlying enum values cooked blobs persist by integer stay stable.
        R32Uint,
        /// @brief BC5 block-compressed two-channel RG, normalized [0,1] (4x4 16-byte blocks).
        ///        The two-channel normal-map codec (X/Y stored, Z reconstructed in-shader).
        ///        Appended at the fixed ordinal 26 for cooked-blob integer stability; the cooker
        ///        writes the literal and the texture loader reads it.
        BC5Unorm,
        /// @brief BC4 block-compressed single-channel R, normalized [0,1] (4x4 8-byte blocks).
        ///        The single-channel mask codec. Appended at the fixed ordinal 27 for cooked-blob
        ///        integer stability; the cooker writes the literal and the texture loader reads it.
        BC4Unorm,
    };

    /// @brief Requested display output mode for the presentable swapchain.
    ///
    /// A preference, not a guarantee: the engine intersects the mode's candidate formats
    /// with the device's reported surface formats and falls back to SDR when an HDR mode
    /// is unavailable. The resolved result is reported by Context::GetActiveDisplayMode().
    enum class DisplayMode : u8
    {
        /// @brief Pick the best available HDR mode, falling back to SDR. The default.
        Auto,
        /// @brief Standard dynamic range: an 8-bit sRGB-nonlinear swapchain.
        SDR,
        /// @brief HDR10: 10-bit Rec.2020 primaries with the ST2084 (PQ) transfer function.
        HDR10,
        /// @brief Extended-range linear sRGB (scRGB / Apple EDR): 16-bit float, values may
        ///        exceed 1.0. The macOS HDR path.
        ExtendedLinear,
    };

    /// @brief Resolved color space of the presentable swapchain images.
    ///
    /// Determines the final transfer encoding the swapchain composite must apply. The
    /// engine resolves this from the requested DisplayMode against device support.
    enum class DisplayColorSpace : u8
    {
        /// @brief sRGB nonlinear (SDR). An _SRGB-format swapchain applies the sRGB transfer
        ///        on store, so the composite writes linear values unencoded.
        SrgbNonlinear,
        /// @brief Rec.2020 primaries with the ST2084 (PQ) transfer function (HDR10). The
        ///        composite must convert primaries and PQ-encode explicitly.
        Hdr10St2084,
        /// @brief Extended-range linear sRGB (scRGB / Apple EDR). Linear values, may exceed
        ///        1.0; the composite writes them unencoded.
        ExtendedLinearSrgb,
    };

    /// @brief Dimensionality of an image resource.
    enum class ImageType : u8
    {
        /// @brief One-dimensional image.
        Type1D,
        /// @brief Two-dimensional image.
        Type2D,
        /// @brief Three-dimensional (volume) image.
        Type3D,
    };

    /// @brief Vulkan image layout; used by barrier derivation.
    enum class ImageLayout : u8
    {
        /// @brief Unknown or discard-on-transition layout.
        Undefined,
        /// @brief Supports all access types.
        General,
        /// @brief Optimal for color attachment read/write.
        ColorAttachment,
        /// @brief Optimal for depth/stencil attachment read/write.
        DepthAttachment,
        /// @brief Optimal for shader-sample reads.
        ShaderReadOnly,
        /// @brief Optimal for transfer source operations.
        TransferSrc,
        /// @brief Optimal for transfer destination operations.
        TransferDst,
        /// @brief Required for swapchain present.
        PresentSrc,
    };

    /// @brief Dimensionality and array structure of an ImageView.
    enum class ImageViewType : u8
    {
        /// @brief 1D image view.
        Type1D,
        /// @brief 2D image view.
        Type2D,
        /// @brief 3D image view.
        Type3D,
        /// @brief Cube-map image view (6 layers).
        Cube,
        /// @brief Array of 1D images.
        Array1D,
        /// @brief Array of 2D images.
        Array2D,
        /// @brief Array of cube-map images.
        CubeArray,
    };

    /// @brief Bitmask of intended GPU usages for an Image.
    enum class ImageUsage : u32
    {
        /// @brief Read by a shader sampler.
        Sampled = 1 << 0,
        /// @brief Read/written as a storage image.
        Storage = 1 << 1,
        /// @brief Used as a color render target.
        ColorAttachment = 1 << 2,
        /// @brief Used as a depth/stencil render target.
        DepthAttachment = 1 << 3,
        /// @brief Source of a copy/blit operation.
        TransferSrc = 1 << 4,
        /// @brief Destination of a copy/blit operation.
        TransferDst = 1 << 5,
    };

    /// @brief Bitmask of intended GPU usages for a Buffer.
    enum class BufferUsage : u32
    {
        /// @brief Vertex buffer binding.
        Vertex = 1 << 0,
        /// @brief Index buffer binding.
        Index = 1 << 1,
        /// @brief Uniform buffer binding.
        Uniform = 1 << 2,
        /// @brief Storage buffer binding.
        Storage = 1 << 3,
        /// @brief Source of a copy operation.
        TransferSrc = 1 << 4,
        /// @brief Destination of a copy operation.
        TransferDst = 1 << 5,
        /// @brief Source of indirect draw/dispatch arguments (vkCmdDrawIndexedIndirect).
        Indirect = 1 << 6,
    };

    /// @brief Bitmask of shader stages.
    enum class ShaderStage : u32
    {
        /// @brief Vertex shader stage.
        Vertex = 1 << 0,
        /// @brief Fragment shader stage.
        Fragment = 1 << 1,
        /// @brief Compute shader stage.
        Compute = 1 << 2,
        /// @brief All stages.
        All = 0xFFFFFFFF,
    };

    /// @brief Load operation applied to an attachment at the start of a render pass.
    enum class LoadOp : u8
    {
        /// @brief Preserve existing attachment contents.
        Load,
        /// @brief Clear to the specified ClearValue.
        Clear,
        /// @brief Contents are undefined (fastest; use when every pixel is overwritten).
        DontCare,
    };

    /// @brief Store operation applied to an attachment at the end of a render pass.
    enum class StoreOp : u8
    {
        /// @brief Write the rendered result to memory.
        Store,
        /// @brief Discard the result (fastest; use for transient depth attachments).
        DontCare,
    };

    /// @brief How a resource is used by a pass or operation.
    ///
    /// Each kind resolves to a (layout, pipeline-stage, access) scope (Backend::ScopeFor)
    /// that drives barrier derivation. Declared on RenderGraph passes; also passed to
    /// CommandBuffer::PrepareForAccess for out-of-graph consumers the graph cannot see.
    enum class AccessKind : u8
    {
        /// @brief Written as a color attachment.
        ColorAttachment,
        /// @brief Read/written as a depth/stencil attachment.
        DepthAttachment,
        /// @brief Sampled by a shader.
        Sample,
        /// @brief Read as a storage image.
        StorageRead,
        /// @brief Written as a storage image.
        StorageWrite,
        /// @brief Source of a transfer operation.
        TransferSrc,
        /// @brief Destination of a transfer operation.
        TransferDst,
        /// @brief Read as indirect draw/dispatch arguments (eDrawIndirect/eIndirectCommandRead).
        IndirectRead,
        /// @brief Read as a storage buffer.
        ///
        /// Distinct from StorageRead, which is a storage-image layout; a buffer has no
        /// layout, so this resolves to a stage/access memory scope with no transition.
        StorageBufferRead,
        /// @brief Written as a storage buffer.
        ///
        /// Distinct from StorageWrite, which is a storage-image layout; a buffer has no
        /// layout, so this resolves to a stage/access memory scope with no transition.
        StorageBufferWrite,
    };

    /// @brief Width of index elements in an index buffer.
    enum class IndexType : u8
    {
        /// @brief 16-bit unsigned integer indices.
        U16,
        /// @brief 32-bit unsigned integer indices.
        U32,
    };

    /// @brief Comparison operator used by depth tests and shadow SampleCmp.
    enum class CompareOp : u8
    {
        /// @brief Always fails.
        Never,
        /// @brief Passes when reference < sample.
        Less,
        /// @brief Passes when reference == sample.
        Equal,
        /// @brief Passes when reference <= sample.
        LessOrEqual,
        /// @brief Passes when reference > sample.
        Greater,
        /// @brief Passes when reference != sample.
        NotEqual,
        /// @brief Passes when reference >= sample.
        GreaterOrEqual,
        /// @brief Always passes.
        Always,
    };

    /// @brief Face-culling mode for rasterization.
    enum class CullMode : u8
    {
        /// @brief No face culling.
        None,
        /// @brief Cull front-facing triangles.
        Front,
        /// @brief Cull back-facing triangles.
        Back,
        /// @brief Cull all triangles (rasterization is skipped).
        FrontAndBack,
    };

    /// @brief Rasterization fill mode.
    enum class PolygonMode : u8
    {
        /// @brief Fill triangles (standard rendering).
        Fill,
        /// @brief Render triangle edges as lines (wireframe).
        Line,
        /// @brief Render vertices as points.
        Point,
    };

    /// @brief Primitive assembly topology for a graphics pipeline.
    enum class PrimitiveTopology : u8
    {
        /// @brief Each three consecutive vertices form one triangle (the default).
        TriangleList,
        /// @brief Each two consecutive vertices form one independent line segment.
        LineList,
    };

    /// @brief Texture filtering mode.
    enum class Filter : u8
    {
        /// @brief Nearest-neighbor filtering (no interpolation).
        Nearest,
        /// @brief Bilinear interpolation between texels.
        Linear,
    };

    /// @brief Mip-level interpolation mode for samplers.
    enum class MipmapMode : u8
    {
        /// @brief Select the nearest mip level.
        Nearest,
        /// @brief Linearly interpolate between adjacent mip levels.
        Linear,
    };

    /// @brief Texture coordinate wrap mode.
    enum class AddressMode : u8
    {
        /// @brief Tile the texture.
        Repeat,
        /// @brief Tile with mirroring on each repeat.
        MirroredRepeat,
        /// @brief Clamp to the last texel at the edge.
        ClampToEdge,
        /// @brief Clamp to the configured border color.
        ClampToBorder,
    };

    /// @brief Border color used with ClampToBorder address mode.
    enum class BorderColor : u8
    {
        /// @brief (0, 0, 0, 0).
        TransparentBlack,
        /// @brief (0, 0, 0, 1).
        OpaqueBlack,
        /// @brief (1, 1, 1, 1).
        OpaqueWhite,
    };

    /// @brief Which pipeline type a descriptor set is bound to.
    enum class PipelineBindPoint : u8
    {
        /// @brief Graphics pipeline.
        Graphics,
        /// @brief Compute pipeline.
        Compute,
    };

    /// @brief Level of a command buffer in the Vulkan command hierarchy.
    enum class CommandBufferLevel : u8
    {
        /// @brief Submitted directly to a queue.
        Primary,
        /// @brief Executed from a primary command buffer.
        Secondary,
    };

    /// @brief Bitmask of command-buffer usage hints.
    enum class CommandBufferUsage : u32
    {
        /// @brief No special usage.
        None = 0,
        /// @brief The buffer is submitted once and then reset or freed.
        OneTimeSubmit = 1 << 0,
    };

    /// @brief Vulkan descriptor resource type.
    enum class DescriptorType : u8
    {
        /// @brief Sampled image combined with a sampler in a single binding.
        CombinedImageSampler,
        /// @brief Sampled image without an embedded sampler.
        SampledImage,
        /// @brief Image accessed as a storage resource.
        StorageImage,
        /// @brief Read-only uniform buffer.
        UniformBuffer,
        /// @brief Read/write storage buffer.
        StorageBuffer,
        /// @brief Plain sampler (no image) — the bindless registry's separate sampler array.
        Sampler,
        /// @brief Uniform buffer whose bound region is selected by a dynamic offset at bind time.
        ///
        /// One buffer holds several regions; the dynamic offset at vkCmdBindDescriptorSets
        /// selects the live one. Used for per-frame-constants ring buffers on plain descriptor sets.
        UniformBufferDynamic,
    };

    /// @brief Source or destination factor for a blend equation.
    enum class BlendFactor : u8
    {
        /// @brief Factor of 0.
        Zero,
        /// @brief Factor of 1.
        One,
        /// @brief Source color channels.
        SrcColor,
        /// @brief 1 minus source color channels.
        OneMinusSrcColor,
        /// @brief Source alpha.
        SrcAlpha,
        /// @brief 1 minus source alpha.
        OneMinusSrcAlpha,
        /// @brief Destination alpha.
        DstAlpha,
        /// @brief 1 minus destination alpha.
        OneMinusDstAlpha,
    };

    /// @brief Arithmetic operator applied in a blend equation.
    enum class BlendOp : u8
    {
        /// @brief Result = Src + Dst.
        Add,
        /// @brief Result = Src − Dst.
        Subtract,
        /// @brief Result = Dst − Src.
        ReverseSubtract,
        /// @brief Result = min(Src, Dst).
        Min,
        /// @brief Result = max(Src, Dst).
        Max,
    };

    /// @brief Per-color-attachment blend state. Construct directly or use a preset.
    struct BlendState
    {
        /// @brief Whether blending is active for this attachment.
        bool Enable = false;
        /// @brief Source color blend factor.
        BlendFactor SrcColorFactor = BlendFactor::One;
        /// @brief Destination color blend factor.
        BlendFactor DstColorFactor = BlendFactor::Zero;
        /// @brief Color blend operation.
        BlendOp ColorOp = BlendOp::Add;
        /// @brief Source alpha blend factor.
        BlendFactor SrcAlphaFactor = BlendFactor::One;
        /// @brief Destination alpha blend factor.
        BlendFactor DstAlphaFactor = BlendFactor::Zero;
        /// @brief Alpha blend operation.
        BlendOp AlphaOp = BlendOp::Add;

        /// @brief Returns an opaque (no-blend) state.
        static BlendState Opaque() { return {}; }

        /// @brief Returns a standard pre-multiplied alpha-blend state.
        static BlendState AlphaBlend()
        {
            return {
                .Enable = true,
                .SrcColorFactor = BlendFactor::SrcAlpha,
                .DstColorFactor = BlendFactor::OneMinusSrcAlpha,
                .ColorOp = BlendOp::Add,
                .SrcAlphaFactor = BlendFactor::One,
                .DstAlphaFactor = BlendFactor::OneMinusSrcAlpha,
                .AlphaOp = BlendOp::Add,
            };
        }

        /// @brief Returns an additive blend state (src + dst), for a pure light-emitting addend.
        static BlendState Additive()
        {
            return {
                .Enable = true,
                .SrcColorFactor = BlendFactor::One,
                .DstColorFactor = BlendFactor::One,
                .ColorOp = BlendOp::Add,
                .SrcAlphaFactor = BlendFactor::One,
                .DstAlphaFactor = BlendFactor::One,
                .AlphaOp = BlendOp::Add,
            };
        }
    };

    /// @brief RGBA clear value for a color attachment.
    struct ClearColor
    {
        /// @brief Red channel clear value.
        f32 R = 0;
        /// @brief Green channel clear value.
        f32 G = 0;
        /// @brief Blue channel clear value.
        f32 B = 0;
        /// @brief Alpha channel clear value (default opaque).
        f32 A = 1;
    };

    /// @brief Depth/stencil clear value for a depth attachment.
    struct ClearDepth
    {
        /// @brief Depth clear value (1.0 = far plane).
        f32 Depth = 1;
        /// @brief Stencil clear value.
        u32 Stencil = 0;
    };

    /// @brief Clear value for a color or depth attachment.
    using ClearValue = std::variant<ClearColor, ClearDepth>;
}

/// @brief Defines bitwise |, &, |= operators and HasFlag for a scoped flags enum.
///
/// Values are stored and compared via the underlying integer type.
#define VE_ENUM_FLAGS(E)                                                                           \
    inline constexpr E operator|(E a, E b)                                                         \
    {                                                                                              \
        return static_cast<E>(static_cast<std::underlying_type_t<E>>(a) |                          \
                              static_cast<std::underlying_type_t<E>>(b));                          \
    }                                                                                              \
    inline constexpr E operator&(E a, E b)                                                         \
    {                                                                                              \
        return static_cast<E>(static_cast<std::underlying_type_t<E>>(a) &                          \
                              static_cast<std::underlying_type_t<E>>(b));                          \
    }                                                                                              \
    inline constexpr E& operator|=(E& a, E b)                                                      \
    {                                                                                              \
        return a = a | b;                                                                          \
    }                                                                                              \
    inline constexpr bool HasFlag(E value, E flag)                                                 \
    {                                                                                              \
        return (static_cast<std::underlying_type_t<E>>(value) &                                    \
                static_cast<std::underlying_type_t<E>>(flag)) != 0;                                \
    }

namespace Veng::Renderer
{
    VE_ENUM_FLAGS(ImageUsage)
    VE_ENUM_FLAGS(BufferUsage)
    VE_ENUM_FLAGS(ShaderStage)
    VE_ENUM_FLAGS(CommandBufferUsage)
}
