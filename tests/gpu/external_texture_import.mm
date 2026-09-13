// Importing an externally-owned platform texture as an engine Image, proven end to end on the one
// platform that has the capability: an IOSurface-backed MTLTexture created on the device the
// backend itself is running on, imported, rendered into from Vulkan, and read back through the
// IOSurface's own base address. Nothing copies the result out of Vulkan — if the bytes are there,
// the render landed in the external memory.
//
// Three claims, one per mapped format plus the refusal:
//   * BGRA8Unorm_sRGB imports as BGRA8Srgb and its store *encodes* — a linear 0.5 clear reads back
//     as the sRGB byte (~188), not as 128. That is the property an 8-bit capture depends on.
//   * BGR10A2Unorm imports as A2R10G10B10Unorm and the packed word carries each channel in the
//     right ten-bit field — the ordering the mapping table asserts and the implementation does not
//     validate.
//   * An unmapped pixel format is refused rather than imported with swapped channels.
//
// The description is read off the texture in every case; the test never tells the import what the
// texture is.

#include <doctest/doctest.h>

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Backend/MetalInterop.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Width = 256;
    constexpr u32 Height = 128;
    constexpr u32 PatchEdge = 16;

    /// @brief Creates an IOSurface of the given pixel format, four bytes per texel.
    IOSurfaceRef MakeSurface(const OSType pixelFormat)
    {
        NSDictionary* properties = @{
            (id)kIOSurfaceWidth : @(Width),
            (id)kIOSurfaceHeight : @(Height),
            (id)kIOSurfaceBytesPerElement : @4,
            (id)kIOSurfacePixelFormat : @(pixelFormat),
        };
        return IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    }

    /// @brief Wraps an IOSurface as an MTLTexture on the backend's own device.
    id<MTLTexture> MakeTexture(void* device, IOSurfaceRef surface, const MTLPixelFormat format)
    {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                               width:Width
                                                              height:Height
                                                           mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        descriptor.storageMode = MTLStorageModeShared;
        return [(id<MTLDevice>)device newTextureWithDescriptor:descriptor
                                                     iosurface:surface
                                                         plane:0];
    }

    /// @brief Records a single clear of @p view through a one-pass render graph.
    void RecordClear(Context& context, CommandBuffer& cmd, const Ref<ImageView>& view,
                     const ClearColor& clear)
    {
        RenderGraph graph(context);
        const ResourceId target = graph.Import("Target");
        graph.AddPass("clear")
            .Color({
                .Resource = target,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = clear,
            })
            .Execute([](PassContext&) {});
        const RenderGraph::ImportBinding binding{.Id = target, .View = view};
        graph.Compile()->Execute(cmd, {&binding, 1});
    }

    /// @brief Reads a texel's four bytes out of a locked IOSurface.
    const u8* TexelAt(const u8* base, const usize stride, const u32 x, const u32 y)
    {
        return base + (y * stride) + (x * 4);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "an imported sRGB external texture stores encoded bytes")
{
    REQUIRE(Context.IsExternalTextureImportSupported());
    void* const device = Context.GetExternalDevice();
    REQUIRE(device != nullptr);

    IOSurfaceRef surface = MakeSurface('BGRA');
    REQUIRE(surface != nullptr);
    id<MTLTexture> texture = MakeTexture(device, surface, MTLPixelFormatBGRA8Unorm_sRGB);
    REQUIRE(texture != nil);

    const CFIndex retainedBefore = CFGetRetainCount((CFTypeRef)texture);

    // The extent and format are read off the texture; nothing here supplies either.
    const optional<Backend::ExternalTextureDescription> description =
        Backend::DescribeExternalTexture(texture);
    REQUIRE(description.has_value());
    CHECK(description->Extent == uvec2{Width, Height});
    CHECK(description->Format == Format::BGRA8Srgb);

    Ref<Image> imported = Backend::ImportExternalTexture(Context, texture, "Imported sRGB");
    REQUIRE(imported != nullptr);
    CHECK(imported->GetFormat() == Format::BGRA8Srgb);
    CHECK(imported->GetExtent() == uvec3{Width, Height, 1});
    CHECK(imported->IsManaged());
    CHECK(CFGetRetainCount((CFTypeRef)texture) > retainedBefore);

    auto importedView =
        ImageView::Create(Context, {.Name = "Imported sRGB View", .Image = imported});

    // A patch blitted in from an engine-owned image, so the import's transfer-destination usage is
    // exercised beside its colour-attachment one.
    const auto patch =
        Image::Create(Context, {
                                   .Name = "Blit Patch",
                                   .Extent = {PatchEdge, PatchEdge, 1},
                                   .Format = Format::BGRA8Srgb,
                                   .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
                               });
    const auto patchView = ImageView::Create(Context, {.Name = "Blit Patch View", .Image = patch});

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RecordClear(Context, cmd, patchView,
                        ClearColor{.R = 1.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f});
            RecordClear(Context, cmd, importedView,
                        ClearColor{.R = 0.5f, .G = 0.5f, .B = 0.5f, .A = 1.0f});

            cmd.PrepareForAccess(patchView, AccessKind::TransferSrc);
            cmd.PrepareForAccess(importedView, AccessKind::TransferDst);
            cmd.BlitImage({
                .SourceImage = patch,
                .DestinationImage = imported,
                .SourceMipLevel = 0,
                .DestinationMipLevel = 0,
                .SourceOffset = {0, 0, 0},
                .DestinationOffset = {0, 0, 0},
                .SourceExtent = {PatchEdge, PatchEdge, 1},
                .DestinationExtent = {PatchEdge, PatchEdge, 1},
            });
        });

    REQUIRE(IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) == kIOReturnSuccess);
    const auto* const base = static_cast<const u8*>(IOSurfaceGetBaseAddress(surface));
    const usize stride = IOSurfaceGetBytesPerRow(surface);
    REQUIRE(base != nullptr);
    CHECK(stride >= static_cast<usize>(Width) * 4);

    // The whole cleared area, accumulated rather than asserted per texel. Linear 0.5 through an
    // sRGB store is ~0.7354, so a byte near 188 says the store encoded and a byte near 128 says it
    // wrote the linear value through untouched — the defect this format exists to avoid.
    u32 clearedMin = 255;
    u32 clearedMax = 0;
    for (u32 y = PatchEdge; y < Height; ++y)
    {
        for (u32 x = PatchEdge; x < Width; ++x)
        {
            const u8* const texel = TexelAt(base, stride, x, y);
            for (u32 channel = 0; channel < 3; ++channel)
            {
                clearedMin = std::min(clearedMin, static_cast<u32>(texel[channel]));
                clearedMax = std::max(clearedMax, static_cast<u32>(texel[channel]));
            }
        }
    }
    CHECK(clearedMin >= 185);
    CHECK(clearedMax <= 190);

    // The patch, in the IOSurface's own BGRA byte order: red at full sRGB, nothing else.
    const u8* const patchTexel = TexelAt(base, stride, PatchEdge / 2, PatchEdge / 2);
    CHECK(patchTexel[0] <= 4);
    CHECK(patchTexel[1] <= 4);
    CHECK(patchTexel[2] >= 250);

    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);

    // Dropping the image retires its VkImage and, behind it, the engine's reference to the texture.
    // The view holds an owning reference of its own, so it goes first; the retire is then
    // fence-deferred, landing once every slot has cycled past the drop.
    importedView.reset();
    imported.reset();
    for (u32 frame = 0; frame <= Context.GetMaxFramesInFlight(); ++frame)
    {
        Context.BeginFrame();
        Context.EndFrame();
    }
    Context.WaitIdle();
    CHECK(CFGetRetainCount((CFTypeRef)texture) == retainedBefore);

    [texture release];
    CFRelease(surface);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "an imported ten-bit external texture packs its channels")
{
    REQUIRE(Context.IsExternalTextureImportSupported());
    void* const device = Context.GetExternalDevice();
    REQUIRE(device != nullptr);

    IOSurfaceRef surface = MakeSurface('l10r');
    REQUIRE(surface != nullptr);
    id<MTLTexture> texture = MakeTexture(device, surface, MTLPixelFormatBGR10A2Unorm);
    REQUIRE(texture != nil);

    const optional<Backend::ExternalTextureDescription> description =
        Backend::DescribeExternalTexture(texture);
    REQUIRE(description.has_value());
    CHECK(description->Format == Format::A2R10G10B10Unorm);

    const Ref<Image> imported = Backend::ImportExternalTexture(Context, texture, "Imported 10-bit");
    REQUIRE(imported != nullptr);

    const auto importedView =
        ImageView::Create(Context, {.Name = "Imported 10-bit View", .Image = imported});

    constexpr f32 Red = 0.25f;
    constexpr f32 Green = 0.5f;
    constexpr f32 Blue = 0.75f;
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd) {
            RecordClear(Context, cmd, importedView,
                        ClearColor{.R = Red, .G = Green, .B = Blue, .A = 1.0f});
        });

    REQUIRE(IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr) == kIOReturnSuccess);
    const auto* const base = static_cast<const u8*>(IOSurfaceGetBaseAddress(surface));
    const usize stride = IOSurfaceGetBytesPerRow(surface);
    REQUIRE(base != nullptr);

    u32 word = 0;
    std::memcpy(&word, TexelAt(base, stride, Width / 2, Height / 2), sizeof(word));
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);

    // A2R10G10B10 packs alpha in the top two bits, then red, green, blue downward. Each channel is
    // checked within one code of its quantisation so the claim is the field order and the ten-bit
    // range, not a particular rounding of the midpoint.
    const auto quantised = [](const f32 value) { return static_cast<i32>(std::lround(value * 1023.0f)); };
    CHECK((word >> 30) == 3u);
    CHECK(std::abs(static_cast<i32>((word >> 20) & 0x3FF) - quantised(Red)) <= 1);
    CHECK(std::abs(static_cast<i32>((word >> 10) & 0x3FF) - quantised(Green)) <= 1);
    CHECK(std::abs(static_cast<i32>(word & 0x3FF) - quantised(Blue)) <= 1);

    [texture release];
    CFRelease(surface);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "an unmapped external pixel format is refused")
{
    REQUIRE(Context.IsExternalTextureImportSupported());
    void* const device = Context.GetExternalDevice();
    REQUIRE(device != nullptr);

    IOSurfaceRef surface = MakeSurface('RGBA');
    REQUIRE(surface != nullptr);
    id<MTLTexture> texture = MakeTexture(device, surface, MTLPixelFormatRGBA8Unorm);
    REQUIRE(texture != nil);

    // No engine format claims this one, and the implementation would accept a mismatched import
    // silently — so the refusal is the only thing standing between a caller and swapped channels.
    CHECK_FALSE(Backend::DescribeExternalTexture(texture).has_value());
    CHECK(Backend::ImportExternalTexture(Context, texture, "Unmapped") == nullptr);

    CHECK_FALSE(Backend::DescribeExternalTexture(nullptr).has_value());

    [texture release];
    CFRelease(surface);
}
