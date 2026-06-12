#pragma once

#include <Veng/Veng.h>
#include <variant>

// Engine-owned vocabulary types. Consumers write these instead of vk::/GLFW/nfd
// types; the backend maps them to Vulkan in TypeMapping.h with exhaustive
// switches. Enumerators are populated on demand — the backend mapping asserts on
// unmapped values, so a missing format/usage is a loud, one-line fix.
namespace Veng::Renderer
{
    enum class Format : u8
    {
        Undefined,
        R8Unorm,
        RGBA8Unorm,
        RGBA8Srgb,
        BGRA8Srgb,
        R16Sfloat,
        RGBA16Sfloat,
        R32Sfloat,
        RG32Sfloat,
        RGB32Sfloat,
        RGBA32Sfloat,
        D16Unorm,
        D32Sfloat,
        S8Uint,
        D16UnormS8Uint,
        D24UnormS8Uint,
        D32SfloatS8Uint,
        X8D24UnormPack32,
    };

    enum class ImageType : u8 { Type1D, Type2D, Type3D };

    enum class ImageUsage : u32
    {
        Sampled = 1 << 0,
        Storage = 1 << 1,
        ColorAttachment = 1 << 2,
        DepthAttachment = 1 << 3,
        TransferSrc = 1 << 4,
        TransferDst = 1 << 5,
    };

    enum class BufferUsage : u32
    {
        Vertex = 1 << 0,
        Index = 1 << 1,
        Uniform = 1 << 2,
        Storage = 1 << 3,
        TransferSrc = 1 << 4,
        TransferDst = 1 << 5,
    };

    enum class ShaderStage : u32
    {
        Vertex = 1 << 0,
        Fragment = 1 << 1,
        Compute = 1 << 2,
        All = 0xFFFFFFFF,
    };

    enum class LoadOp : u8 { Load, Clear, DontCare };
    enum class StoreOp : u8 { Store, DontCare };

    enum class CompareOp : u8
    {
        Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always,
    };

    enum class CullMode : u8 { None, Front, Back, FrontAndBack };
    enum class PolygonMode : u8 { Fill, Line, Point };

    enum class Filter : u8 { Nearest, Linear };
    enum class MipmapMode : u8 { Nearest, Linear };
    enum class AddressMode : u8 { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };
    enum class BorderColor : u8 { TransparentBlack, OpaqueBlack, OpaqueWhite };

    enum class PipelineBindPoint : u8 { Graphics, Compute };

    enum class DescriptorType : u8
    {
        CombinedImageSampler, SampledImage, StorageImage, UniformBuffer, StorageBuffer,
    };

    enum class BlendFactor : u8
    {
        Zero, One, SrcColor, OneMinusSrcColor, SrcAlpha, OneMinusSrcAlpha,
        DstAlpha, OneMinusDstAlpha,
    };

    enum class BlendOp : u8 { Add, Subtract, ReverseSubtract, Min, Max };

    // Per-color-attachment blend state. Construct directly or use a preset.
    struct BlendState
    {
        bool Enable = false;
        BlendFactor SrcColorFactor = BlendFactor::One;
        BlendFactor DstColorFactor = BlendFactor::Zero;
        BlendOp ColorOp = BlendOp::Add;
        BlendFactor SrcAlphaFactor = BlendFactor::One;
        BlendFactor DstAlphaFactor = BlendFactor::Zero;
        BlendOp AlphaOp = BlendOp::Add;

        static BlendState Opaque() { return {}; }

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
    };

    struct ClearColor { f32 R = 0, G = 0, B = 0, A = 1; };
    struct ClearDepth { f32 Depth = 1; u32 Stencil = 0; };
    using ClearValue = std::variant<ClearColor, ClearDepth>;
}

// Bitwise operators + HasFlag for a scoped flags enum. Values stored/compared
// via the underlying type.
#define VE_ENUM_FLAGS(E)                                                                   \
    inline constexpr E operator|(E a, E b)                                                 \
    {                                                                                      \
        return static_cast<E>(static_cast<std::underlying_type_t<E>>(a) |                  \
                              static_cast<std::underlying_type_t<E>>(b));                  \
    }                                                                                      \
    inline constexpr E operator&(E a, E b)                                                 \
    {                                                                                      \
        return static_cast<E>(static_cast<std::underlying_type_t<E>>(a) &                  \
                              static_cast<std::underlying_type_t<E>>(b));                  \
    }                                                                                      \
    inline constexpr E& operator|=(E& a, E b) { return a = a | b; }                        \
    inline constexpr bool HasFlag(E value, E flag)                                         \
    {                                                                                      \
        return (static_cast<std::underlying_type_t<E>>(value) &                            \
                static_cast<std::underlying_type_t<E>>(flag)) != 0;                        \
    }

namespace Veng::Renderer
{
    VE_ENUM_FLAGS(ImageUsage)
    VE_ENUM_FLAGS(BufferUsage)
    VE_ENUM_FLAGS(ShaderStage)
}
