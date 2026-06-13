// Type-mapping round-trips (planset-3, plan 03): lock down the vocabulary-enum
// ↔ Vulkan switches in Backend/TypeMapping.h. A wrong or missing mapping is
// silent UB at the backend boundary; here it becomes a failing case.
//
// This TU includes a *backend* header (TypeMapping.h → vk::), which is allowed
// for tests — the include-hygiene guard is specifically about *public* headers
// leaking backend types. The mappings are pure inline functions: no device, no
// driver. veng_unit is given the Vulkan/GLFW/VMA include dirs for this TU; that
// is headers only, no run-time driver dependency.
//
// The per-enum arrays below must track Renderer/Types.h: they drive the
// round-trip / coverage loops, so a new enumerator is one edit here. (The
// "unmapped enumerator aborts" path is a death, covered in plan 05; here we
// assert the *currently defined* enumerators all map correctly.)

#include <doctest/doctest.h>

#include <array>
#include <set>
#include <type_traits>

#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/Backend/TypeMapping.h>

using namespace Veng::Renderer;

namespace
{
    // Underlying-value set, for asserting a scalar ToVk produces distinct
    // outputs across an enum (a duplicated switch arm would collapse two).
    template <typename Vk>
    auto Underlying(Vk v) { return static_cast<std::underlying_type_t<Vk>>(v); }
}

// --- Round-trip enums (have a FromVk inverse) -------------------------------

TEST_CASE("Format round-trips through ToVk/FromVk")
{
    constexpr std::array formats = {
        Format::Undefined, Format::R8Unorm, Format::RGBA8Unorm, Format::RGBA8Srgb,
        Format::BGRA8Srgb, Format::R16Sfloat, Format::RGBA16Sfloat, Format::R32Sfloat,
        Format::RG32Sfloat, Format::RGB32Sfloat, Format::RGBA32Sfloat, Format::D16Unorm,
        Format::D32Sfloat, Format::S8Uint, Format::D16UnormS8Uint, Format::D24UnormS8Uint,
        Format::D32SfloatS8Uint, Format::X8D24UnormPack32,
    };

    std::set<std::underlying_type_t<vk::Format>> distinct;
    for (Format f : formats)
    {
        CHECK(FromVk(ToVk(f)) == f);     // asymmetric edit to one direction fails here
        distinct.insert(Underlying(ToVk(f)));
    }
    CHECK(distinct.size() == formats.size()); // no two engine formats collide in vk

    // Spot-check a couple of concrete mappings.
    CHECK(ToVk(Format::RGBA8Unorm) == vk::Format::eR8G8B8A8Unorm);
    CHECK(ToVk(Format::D32Sfloat) == vk::Format::eD32Sfloat);
}

TEST_CASE("ImageLayout round-trips through ToVk/FromVk")
{
    constexpr std::array layouts = {
        ImageLayout::Undefined, ImageLayout::General, ImageLayout::ColorAttachment,
        ImageLayout::DepthAttachment, ImageLayout::ShaderReadOnly, ImageLayout::TransferSrc,
        ImageLayout::TransferDst, ImageLayout::PresentSrc,
    };

    std::set<std::underlying_type_t<vk::ImageLayout>> distinct;
    for (ImageLayout l : layouts)
    {
        CHECK(FromVk(ToVk(l)) == l);
        distinct.insert(Underlying(ToVk(l)));
    }
    CHECK(distinct.size() == layouts.size());

    CHECK(ToVk(ImageLayout::ColorAttachment) == vk::ImageLayout::eColorAttachmentOptimal);
    CHECK(ToVk(ImageLayout::PresentSrc) == vk::ImageLayout::ePresentSrcKHR);
}

// --- Scalar ToVk enums (no inverse): all defined values map, distinctly -----

TEST_CASE("scalar ToVk enums map every defined enumerator distinctly")
{
    {
        constexpr std::array v = {ImageType::Type1D, ImageType::Type2D, ImageType::Type3D};
        std::set<std::underlying_type_t<vk::ImageType>> s;
        for (auto e : v) s.insert(Underlying(ToVk(e)));
        CHECK(s.size() == v.size());
        CHECK(ToVk(ImageType::Type2D) == vk::ImageType::e2D);
    }
    {
        constexpr std::array v = {
            ImageViewType::Type1D, ImageViewType::Type2D, ImageViewType::Type3D,
            ImageViewType::Cube, ImageViewType::Array1D, ImageViewType::Array2D,
            ImageViewType::CubeArray,
        };
        std::set<std::underlying_type_t<vk::ImageViewType>> s;
        for (auto e : v) s.insert(Underlying(ToVk(e)));
        CHECK(s.size() == v.size());
        CHECK(ToVk(ImageViewType::CubeArray) == vk::ImageViewType::eCubeArray);
    }
    {
        CHECK(ToVk(IndexType::U16) == vk::IndexType::eUint16);
        CHECK(ToVk(IndexType::U32) == vk::IndexType::eUint32);
    }
    {
        constexpr std::array v = {
            CompareOp::Never, CompareOp::Less, CompareOp::Equal, CompareOp::LessOrEqual,
            CompareOp::Greater, CompareOp::NotEqual, CompareOp::GreaterOrEqual, CompareOp::Always,
        };
        std::set<std::underlying_type_t<vk::CompareOp>> s;
        for (auto e : v) s.insert(Underlying(ToVk(e)));
        CHECK(s.size() == v.size());
        CHECK(ToVk(CompareOp::LessOrEqual) == vk::CompareOp::eLessOrEqual);
    }
    {
        CHECK(ToVk(PolygonMode::Fill) == vk::PolygonMode::eFill);
        CHECK(ToVk(PolygonMode::Line) == vk::PolygonMode::eLine);
        CHECK(ToVk(PolygonMode::Point) == vk::PolygonMode::ePoint);
    }
    {
        CHECK(ToVk(Filter::Nearest) == vk::Filter::eNearest);
        CHECK(ToVk(Filter::Linear) == vk::Filter::eLinear);
        CHECK(ToVk(MipmapMode::Linear) == vk::SamplerMipmapMode::eLinear);
    }
    {
        constexpr std::array v = {
            AddressMode::Repeat, AddressMode::MirroredRepeat,
            AddressMode::ClampToEdge, AddressMode::ClampToBorder,
        };
        std::set<std::underlying_type_t<vk::SamplerAddressMode>> s;
        for (auto e : v) s.insert(Underlying(ToVk(e)));
        CHECK(s.size() == v.size());
        CHECK(ToVk(AddressMode::ClampToEdge) == vk::SamplerAddressMode::eClampToEdge);
    }
    {
        CHECK(ToVk(BorderColor::TransparentBlack) == vk::BorderColor::eFloatTransparentBlack);
        CHECK(ToVk(BorderColor::OpaqueWhite) == vk::BorderColor::eFloatOpaqueWhite);
    }
    {
        CHECK(ToVk(PipelineBindPoint::Graphics) == vk::PipelineBindPoint::eGraphics);
        CHECK(ToVk(PipelineBindPoint::Compute) == vk::PipelineBindPoint::eCompute);
    }
    {
        constexpr std::array v = {
            DescriptorType::CombinedImageSampler, DescriptorType::SampledImage,
            DescriptorType::StorageImage, DescriptorType::UniformBuffer,
            DescriptorType::StorageBuffer,
        };
        std::set<std::underlying_type_t<vk::DescriptorType>> s;
        for (auto e : v) s.insert(Underlying(ToVk(e)));
        CHECK(s.size() == v.size());
        CHECK(ToVk(DescriptorType::StorageImage) == vk::DescriptorType::eStorageImage);
    }
    {
        constexpr std::array v = {
            BlendFactor::Zero, BlendFactor::One, BlendFactor::SrcColor,
            BlendFactor::OneMinusSrcColor, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha,
            BlendFactor::DstAlpha, BlendFactor::OneMinusDstAlpha,
        };
        std::set<std::underlying_type_t<vk::BlendFactor>> s;
        for (auto e : v) s.insert(Underlying(ToVk(e)));
        CHECK(s.size() == v.size());
        CHECK(ToVk(BlendFactor::SrcAlpha) == vk::BlendFactor::eSrcAlpha);
    }
    {
        constexpr std::array v = {
            BlendOp::Add, BlendOp::Subtract, BlendOp::ReverseSubtract, BlendOp::Min, BlendOp::Max,
        };
        std::set<std::underlying_type_t<vk::BlendOp>> s;
        for (auto e : v) s.insert(Underlying(ToVk(e)));
        CHECK(s.size() == v.size());
        CHECK(ToVk(BlendOp::ReverseSubtract) == vk::BlendOp::eReverseSubtract);
    }
    {
        CHECK(ToVk(LoadOp::Clear) == vk::AttachmentLoadOp::eClear);
        CHECK(ToVk(LoadOp::Load) == vk::AttachmentLoadOp::eLoad);
        CHECK(ToVk(StoreOp::Store) == vk::AttachmentStoreOp::eStore);
        CHECK(ToVk(StoreOp::DontCare) == vk::AttachmentStoreOp::eDontCare);
    }
    {
        CHECK(ToVk(CommandBufferLevel::Primary) == vk::CommandBufferLevel::ePrimary);
        CHECK(ToVk(CommandBufferLevel::Secondary) == vk::CommandBufferLevel::eSecondary);
    }
    {
        // CullMode maps to a Flags wrapper; check the bits directly.
        CHECK(ToVk(CullMode::None) == vk::CullModeFlags{vk::CullModeFlagBits::eNone});
        CHECK(ToVk(CullMode::Back) == vk::CullModeFlags{vk::CullModeFlagBits::eBack});
        CHECK(ToVk(CullMode::FrontAndBack) == vk::CullModeFlags{vk::CullModeFlagBits::eFrontAndBack});
    }
}

// --- Flag combiners (bitwise OR mappers) ------------------------------------

TEST_CASE("ImageUsage flags combine")
{
    CHECK(ToVk(ImageUsage::Sampled) == vk::ImageUsageFlagBits::eSampled);
    CHECK(ToVk(ImageUsage::Storage) == vk::ImageUsageFlagBits::eStorage);

    const vk::ImageUsageFlags combined = ToVk(ImageUsage::Sampled | ImageUsage::ColorAttachment);
    CHECK((combined & vk::ImageUsageFlagBits::eSampled));
    CHECK((combined & vk::ImageUsageFlagBits::eColorAttachment));
    CHECK_FALSE((combined & vk::ImageUsageFlagBits::eStorage));
    CHECK(combined == (vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment));
}

TEST_CASE("BufferUsage flags combine")
{
    CHECK(ToVk(BufferUsage::Vertex) == vk::BufferUsageFlagBits::eVertexBuffer);

    const vk::BufferUsageFlags combined = ToVk(BufferUsage::Index | BufferUsage::TransferDst);
    CHECK(combined == (vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eTransferDst));
}

TEST_CASE("ShaderStage flags combine, with the All shortcut")
{
    CHECK(ToVk(ShaderStage::Vertex) == vk::ShaderStageFlagBits::eVertex);

    const vk::ShaderStageFlags vf = ToVk(ShaderStage::Vertex | ShaderStage::Fragment);
    CHECK(vf == (vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment));

    // The All special-case is handled before the per-bit OR.
    CHECK(ToVk(ShaderStage::All) == vk::ShaderStageFlags{vk::ShaderStageFlagBits::eAll});

    // Single-stage variant.
    CHECK(ToVkBit(ShaderStage::Compute) == vk::ShaderStageFlagBits::eCompute);
}

// --- Composite mappers ------------------------------------------------------

TEST_CASE("BlendState composite mapper")
{
    const auto opaque = ToVk(BlendState::Opaque());
    CHECK(opaque.blendEnable == vk::False);

    const auto alpha = ToVk(BlendState::AlphaBlend());
    CHECK(alpha.blendEnable == vk::True);
    CHECK(alpha.srcColorBlendFactor == vk::BlendFactor::eSrcAlpha);
    CHECK(alpha.dstColorBlendFactor == vk::BlendFactor::eOneMinusSrcAlpha);
    CHECK(alpha.colorBlendOp == vk::BlendOp::eAdd);
    CHECK(alpha.srcAlphaBlendFactor == vk::BlendFactor::eOne);
    CHECK(alpha.dstAlphaBlendFactor == vk::BlendFactor::eOneMinusSrcAlpha);
    // Full RGBA write mask regardless of blend settings.
    CHECK(alpha.colorWriteMask == (vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                   vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA));
}

TEST_CASE("ClearValue composite mapper")
{
    const vk::ClearValue color = ToVk(ClearValue{ClearColor{0.25f, 0.5f, 0.75f, 1.0f}});
    CHECK(color.color.float32[0] == doctest::Approx(0.25f));
    CHECK(color.color.float32[1] == doctest::Approx(0.5f));
    CHECK(color.color.float32[2] == doctest::Approx(0.75f));
    CHECK(color.color.float32[3] == doctest::Approx(1.0f));

    const vk::ClearValue depth = ToVk(ClearValue{ClearDepth{0.5f, 3}});
    CHECK(depth.depthStencil.depth == doctest::Approx(0.5f));
    CHECK(depth.depthStencil.stencil == 3u);
}
