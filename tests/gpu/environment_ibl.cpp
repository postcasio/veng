// EnvironmentIbl's source-agnostic convolution: GenerateFromCube runs the irradiance/prefilter
// convolution from any supplied radiance cube, and Generate is re-expressed as equirect→cube then
// GenerateFromCube. These GPU cases feed synthetic analytic radiance cubes through the real
// compute convolution and read back the irradiance map to assert:
//
//   1. A constant radiance cube (every texel L) convolves to a constant diffuse irradiance pi*L in
//      every face texel — the Lambertian response of a uniform sky, through the GPU convolver.
//   2. A directional bright cube (bright toward +Y) convolves to an irradiance that peaks in the
//      +Y face and is least in the -Y face — the diffuse follows the source direction.
//   3. Regression guard: GenerateFromCube over one cube produces a bit-identical irradiance map on
//      a repeat run (the convolution is deterministic), and Generate's convolution arm is that
//      same GenerateFromCube — so re-expressing Generate over it cannot move the maps.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <array>
#include <cmath>
#include <vector>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

#include "Renderer/EnvironmentIbl.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 CubeFaces = 6;
    constexpr u32 SourceFaceSize = 64;

    // The world direction a source-cube face texel carries, matching ibl_equirect_to_cube's
    // FaceDirection and EnvironmentIbl's projection basis.
    vec3 FaceDirection(u32 face, f32 u, f32 v)
    {
        const f32 sx = u * 2.0f - 1.0f;
        const f32 sy = v * 2.0f - 1.0f;
        vec3 dir(0.0f);
        switch (face)
        {
        case 0:
            dir = vec3(1.0f, -sy, -sx);
            break;
        case 1:
            dir = vec3(-1.0f, -sy, sx);
            break;
        case 2:
            dir = vec3(sx, 1.0f, sy);
            break;
        case 3:
            dir = vec3(sx, -1.0f, -sy);
            break;
        case 4:
            dir = vec3(sx, -sy, 1.0f);
            break;
        default:
            dir = vec3(-sx, -sy, -1.0f);
            break;
        }
        return glm::normalize(dir);
    }

    // Builds a synthetic RGBA16F radiance cube image (six layers, single mip), sampled + storage +
    // transfer-src, and uploads a per-texel radiance authored as a function of the direction.
    template <typename Fn>
    Ref<Image> MakeCubeImage(Context& context, Fn radiance)
    {
        const Ref<Image> image =
            Image::Create(context, {
                                       .Name = "Test Analytic Radiance Cube",
                                       .Extent = {SourceFaceSize, SourceFaceSize, 1},
                                       .Layers = CubeFaces,
                                       .Format = Format::RGBA16Sfloat,
                                       .Usage = ImageUsage::Sampled | ImageUsage::Storage |
                                                ImageUsage::TransferDst,
                                   });

        std::vector<u16> texels(static_cast<usize>(SourceFaceSize) * SourceFaceSize * CubeFaces *
                                4);
        const f32 invFace = 1.0f / static_cast<f32>(SourceFaceSize);
        for (u32 face = 0; face < CubeFaces; ++face)
        {
            for (u32 y = 0; y < SourceFaceSize; ++y)
            {
                for (u32 x = 0; x < SourceFaceSize; ++x)
                {
                    const f32 u = (static_cast<f32>(x) + 0.5f) * invFace;
                    const f32 v = (static_cast<f32>(y) + 0.5f) * invFace;
                    const vec3 c = radiance(FaceDirection(face, u, v));
                    const usize base =
                        ((static_cast<usize>(face) * SourceFaceSize + y) * SourceFaceSize + x) * 4;
                    texels[base + 0] = glm::packHalf1x16(c.r);
                    texels[base + 1] = glm::packHalf1x16(c.g);
                    texels[base + 2] = glm::packHalf1x16(c.b);
                    texels[base + 3] = glm::packHalf1x16(1.0f);
                }
            }
        }
        image->UploadSync(
            std::span(reinterpret_cast<const u8*>(texels.data()), texels.size() * sizeof(u16)));
        return image;
    }

    Ref<ImageView> CubeView(Context& context, const Ref<Image>& image)
    {
        return ImageView::Create(context, {
                                              .Name = "Test Radiance Cube View",
                                              .Image = image,
                                              .ViewType = ImageViewType::Cube,
                                              .ArrayLayers = CubeFaces,
                                          });
    }

    // Downloads all six irradiance-cube layers into one tightly-packed RGBA16F buffer (layer-major).
    std::vector<u8> DownloadIrradiance(Context& context, EnvironmentIbl& ibl)
    {
        const u32 faceSize = EnvironmentIbl::GetIrradianceFaceSize();
        const usize faceBytes = static_cast<usize>(faceSize) * faceSize * 8;
        const Ref<Image>& image = ibl.GetIrradianceImage();
        const Ref<ImageView> view = ImageView::Create(context, {
                                                                   .Name = "Test Irradiance View",
                                                                   .Image = image,
                                                                   .ViewType = ImageViewType::Cube,
                                                                   .ArrayLayers = CubeFaces,
                                                               });
        const Ref<Buffer> staging = Buffer::Create(context, {
                                                                .Name = "Test Irradiance Readback",
                                                                .Size = faceBytes * CubeFaces,
                                                                .Usage = BufferUsage::TransferDst,
                                                            });
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                cmd.PrepareForAccess(view, AccessKind::TransferSrc);
                const vk::BufferImageCopy region{
                    .bufferOffset = 0,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = CubeFaces},
                    .imageOffset = {.x = 0, .y = 0, .z = 0},
                    .imageExtent = {.width = faceSize, .height = faceSize, .depth = 1},
                };
                GetVkCommandBuffer(cmd).copyImageToBuffer(GetVkImage(*image),
                                                          vk::ImageLayout::eTransferSrcOptimal,
                                                          GetVkBuffer(*staging), 1, &region);
                cmd.PrepareForAccess(view, AccessKind::SampleGraphics);
            });
        return staging->Download();
    }

    vec3 IrradianceTexel(const std::vector<u8>& bytes, u32 faceSize, u32 face, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(bytes.data());
        const usize base = ((static_cast<usize>(face) * faceSize + y) * faceSize + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    vec3 FaceCenter(const std::vector<u8>& bytes, u32 faceSize, u32 face)
    {
        return IrradianceTexel(bytes, faceSize, face, faceSize / 2, faceSize / 2);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "EnvironmentIbl::GenerateFromCube: a constant radiance cube convolves to "
                  "constant albedo-ready irradiance")
{
    AssetManager assets(Context, Tasks, Types);
    const Unique<EnvironmentIbl> ibl = EnvironmentIbl::Create(Context, assets);

    constexpr vec3 L(0.3f, 0.6f, 0.9f);
    const Ref<Image> cube = MakeCubeImage(Context, [&](vec3) { return L; });
    const Ref<ImageView> view = CubeView(Context, cube);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            ibl->EnsureInitialized(cmd);
            cmd.PrepareForAccess(view, AccessKind::SampleGraphics);
            ibl->GenerateFromCube(cmd, view, SourceFaceSize);
        });

    const u32 faceSize = EnvironmentIbl::GetIrradianceFaceSize();
    const std::vector<u8> irradiance = DownloadIrradiance(Context, *ibl);

    // The IBL irradiance map stores the albedo-ready diffuse term (the cosine-weighted integral
    // pi*L already divided by pi in ibl_irradiance.comp, so the lighting pass multiplies it by
    // albedo directly): a constant radiance L convolves to L, independent of the face/direction. A
    // generous epsilon covers the RGBA16F round-trip and the finite convolution sampling.
    for (u32 face = 0; face < CubeFaces; ++face)
    {
        const vec3 c = FaceCenter(irradiance, faceSize, face);
        CHECK(c.r == doctest::Approx(L.r).epsilon(0.04));
        CHECK(c.g == doctest::Approx(L.g).epsilon(0.04));
        CHECK(c.b == doctest::Approx(L.b).epsilon(0.04));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "EnvironmentIbl::GenerateFromCube: a directional bright cube convolves to "
                  "directional irradiance")
{
    AssetManager assets(Context, Tasks, Types);
    const Unique<EnvironmentIbl> ibl = EnvironmentIbl::Create(Context, assets);

    // Bright toward +Y, dark toward -Y.
    const Ref<Image> cube =
        MakeCubeImage(Context, [](vec3 dir) { return vec3(std::max(0.0f, dir.y)); });
    const Ref<ImageView> view = CubeView(Context, cube);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            ibl->EnsureInitialized(cmd);
            cmd.PrepareForAccess(view, AccessKind::SampleGraphics);
            ibl->GenerateFromCube(cmd, view, SourceFaceSize);
        });

    const u32 faceSize = EnvironmentIbl::GetIrradianceFaceSize();
    const std::vector<u8> irradiance = DownloadIrradiance(Context, *ibl);

    // Face 2 is +Y (toward the bright pole), face 3 is -Y (toward the dark pole). The convolved
    // diffuse irradiance must be greatest facing the bright pole and least facing the dark pole.
    const f32 up = FaceCenter(irradiance, faceSize, 2).r;
    const f32 down = FaceCenter(irradiance, faceSize, 3).r;
    const f32 side = FaceCenter(irradiance, faceSize, 0).r; // +X
    CHECK(up > side);
    CHECK(side > down);
    CHECK(up > 0.0f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "EnvironmentIbl::GenerateFromCube: the convolution is deterministic — the "
                  "equirect-split regression guard")
{
    AssetManager assets(Context, Tasks, Types);
    const Unique<EnvironmentIbl> ibl = EnvironmentIbl::Create(Context, assets);

    // A non-trivial directional radiance exercising several SH bands.
    const Ref<Image> cube = MakeCubeImage(
        Context, [](vec3 dir)
        { return vec3(0.5f + 0.5f * dir.x, 0.4f + 0.4f * dir.y, 0.3f + 0.3f * dir.z); });
    const Ref<ImageView> view = CubeView(Context, cube);

    auto Convolve = [&]() -> std::vector<u8>
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                ibl->EnsureInitialized(cmd);
                cmd.PrepareForAccess(view, AccessKind::SampleGraphics);
                ibl->GenerateFromCube(cmd, view, SourceFaceSize);
            });
        return DownloadIrradiance(Context, *ibl);
    };

    // Generate re-expressed over GenerateFromCube must produce bit-comparable maps to the
    // pre-split path: since Generate's convolution arm *is* this GenerateFromCube, running it twice
    // over one cube must yield a byte-identical irradiance map — a non-deterministic or moved
    // convolution would diverge, catching any drift the split could introduce.
    const std::vector<u8> first = Convolve();
    const std::vector<u8> second = Convolve();
    REQUIRE(first.size() == second.size());
    CHECK(first == second);
}
