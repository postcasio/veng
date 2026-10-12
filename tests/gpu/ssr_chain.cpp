// The screen-space reflection chain on a real device, driven on a synthetic g-buffer in a graph of
// its own so every input texel is known:
//   - every depth-hierarchy texel is the nearest (reverse-Z: largest) of its parent level's 2×2
//     children, a child past the parent's extent counting as 0, at odd extents too;
//   - the classifier lists exactly the pixels its rule selects, each once with its quad-copy flags,
//     for 1, 2 and 4 samples per quad and with variance-guided tracing picking up a skipped pixel;
//     the tile list holds exactly the tiles holding a glossy reflective pixel; the indirect
//     arguments and the rotated counters are the reference formula of those counts; a rough
//     reflective pixel holds the environment times the IBL factor and every other pixel zero;
//   - the blue-noise tile is the sampler's formula over the cooked tables, within [0, 1];
//   - on a mirror floor beside a small lit sphere, the intersection's radiance is finite and
//     non-negative everywhere, a ray leaving the screen holds exactly the environment term, and
//     the floor reflects the sphere; on a glossy floor every quad copy holds its base ray's value;
//   - a renderer with SSR off records none of the chain's passes, and with it on records them all
//     and leaves finite radiance, with temporal anti-aliasing off and on.
// The expected sets are the reference's selection rule, written out per pixel.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include <glm/gtc/packing.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/RawAsset.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

#include "Renderer/SsrChain.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr AssetId BlueNoiseTablesId{0xFC0C39061564BE4BULL};

    // The four kinds of pixel the synthetic g-buffer holds, chosen per pixel by a hash so every
    // quad and tile mixes them. Perceptual roughness 0 is a mirror (alpha 0), 0.4 glossy (alpha
    // 0.16 under the 0.2 threshold), 0.8 rough (alpha 0.64).
    enum class Kind : u8
    {
        Background,
        Mirror,
        Glossy,
        Rough,
    };

    constexpr std::array<u8, 4> PerceptualByKind{0, 0, 102, 204};

    u32 Hash(const u32 x, const u32 y)
    {
        u32 h = x * 0x9E3779B1u ^ (y + 0x7F4A7C15u) * 0x85EBCA77u;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        return h;
    }

    // A tile of 8×8 pixels holding nothing glossy (all background or rough), so the tile list
    // must leave it out.
    bool IsDullTile(const u32 x, const u32 y)
    {
        return (x / 8) == 1 && (y / 8) == 1;
    }

    Kind KindAt(const u32 x, const u32 y)
    {
        const u32 h = Hash(x, y);
        if (IsDullTile(x, y))
        {
            return (h & 1u) != 0u ? Kind::Rough : Kind::Background;
        }
        return static_cast<Kind>(h % 4u);
    }

    f32 AlphaOf(const Kind kind)
    {
        const f32 perceptual =
            static_cast<f32>(PerceptualByKind[static_cast<usize>(kind)]) / 255.0f;
        return perceptual * perceptual;
    }

    // Variance history 1 on roughly a third of the pixels, 0 elsewhere.
    bool HasVariance(const u32 x, const u32 y)
    {
        return (Hash(y, x) % 3u) == 0u;
    }

    struct ExpectedClassification
    {
        std::vector<u32> Rays;  // packed, sorted
        std::vector<u32> Tiles; // packed, sorted
    };

    bool IsBaseRay(const u32 x, const u32 y, const u32 samplesPerQuad)
    {
        switch (samplesPerQuad)
        {
        case 1:
            return ((x & 1u) | (y & 1u)) == 0u;
        case 2:
            return (x & 1u) == (y & 1u);
        default:
            return true;
        }
    }

    // The reference's per-pixel rule (ffx_sssr_classify_tiles.h) over the synthetic g-buffer.
    ExpectedClassification Classify(const uvec2 extent, const u32 samplesPerQuad, const bool guided,
                                    const f32 varianceThreshold)
    {
        const auto Inside = [extent](const u32 x, const u32 y)
        { return x < extent.x && y < extent.y; };
        const auto Reflective = [&](const u32 x, const u32 y)
        { return Inside(x, y) && KindAt(x, y) != Kind::Background; };
        const auto Glossy = [&](const u32 x, const u32 y)
        { return Inside(x, y) && AlphaOf(KindAt(x, y)) < 0.2f; };
        const auto NeedsDenoiser = [&](const u32 x, const u32 y)
        {
            return Reflective(x, y) && Glossy(x, y) &&
                   !(Inside(x, y) && AlphaOf(KindAt(x, y)) < 0.0001f);
        };
        const auto NeedsRay = [&](const u32 x, const u32 y)
        {
            bool needsRay = Reflective(x, y) && Glossy(x, y);
            const bool denoise = NeedsDenoiser(x, y);
            needsRay = needsRay && (!denoise || IsBaseRay(x, y, samplesPerQuad));
            if (guided && denoise && !needsRay)
            {
                needsRay = (HasVariance(x, y) ? 1.0f : 0.0f) > varianceThreshold;
            }
            return needsRay;
        };
        const auto RequiresCopy = [&](const u32 x, const u32 y)
        { return !NeedsRay(x, y) && NeedsDenoiser(x, y); };

        ExpectedClassification expected;
        const uvec2 tiles = (extent + 7u) / 8u;
        for (u32 y = 0; y < tiles.y * 8; ++y)
        {
            for (u32 x = 0; x < tiles.x * 8; ++x)
            {
                if (!NeedsRay(x, y))
                {
                    continue;
                }
                const bool base = IsBaseRay(x, y, samplesPerQuad);
                const bool h = RequiresCopy(x ^ 1u, y) && samplesPerQuad != 4 && base;
                const bool v = RequiresCopy(x, y ^ 1u) && samplesPerQuad == 1 && base;
                const bool d = RequiresCopy(x ^ 1u, y ^ 1u) && samplesPerQuad == 1 && base;
                expected.Rays.push_back((d ? 1u << 31 : 0u) | (v ? 1u << 30 : 0u) |
                                        (h ? 1u << 29 : 0u) | ((y & 0x3FFFu) << 15) |
                                        (x & 0x7FFFu));
            }
        }
        for (u32 ty = 0; ty < tiles.y; ++ty)
        {
            for (u32 tx = 0; tx < tiles.x; ++tx)
            {
                bool any = false;
                for (u32 y = ty * 8; y < ty * 8 + 8; ++y)
                {
                    for (u32 x = tx * 8; x < tx * 8 + 8; ++x)
                    {
                        any = any || (Reflective(x, y) && Glossy(x, y));
                    }
                }
                if (any)
                {
                    expected.Tiles.push_back(((ty * 8) << 16) | (tx * 8));
                }
            }
        }
        std::ranges::sort(expected.Rays);
        std::ranges::sort(expected.Tiles);
        return expected;
    }

    std::vector<u32> Words(const std::vector<u8>& bytes)
    {
        std::vector<u32> words(bytes.size() / sizeof(u32));
        std::memcpy(words.data(), bytes.data(), words.size() * sizeof(u32));
        return words;
    }

    // Reads one level of an R32F image back, through the level's own view.
    std::vector<f32> DownloadLevel(Context& context, const Ref<Image>& image,
                                   const Ref<ImageView>& view, const u32 level, const uvec2 extent)
    {
        const Ref<Buffer> buffer =
            Buffer::Create(context, {
                                        .Name = "SSR Test Level Readback",
                                        .Size = static_cast<u64>(extent.x) * extent.y * sizeof(f32),
                                        .Usage = BufferUsage::TransferDst,
                                    });
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                cmd.PrepareForAccess(view, AccessKind::TransferSrc);
                const vk::BufferImageCopy region{
                    .bufferOffset = 0,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = level,
                                         .baseArrayLayer = 0,
                                         .layerCount = 1},
                    .imageOffset = {.x = 0, .y = 0, .z = 0},
                    .imageExtent = {.width = extent.x, .height = extent.y, .depth = 1},
                };
                GetVkCommandBuffer(cmd).copyImageToBuffer(GetVkImage(*image),
                                                          vk::ImageLayout::eTransferSrcOptimal,
                                                          GetVkBuffer(*buffer), 1, &region);
            });
        const std::vector<u8> bytes = buffer->Download();
        std::vector<f32> values(static_cast<usize>(extent.x) * extent.y);
        std::memcpy(values.data(), bytes.data(), values.size() * sizeof(f32));
        return values;
    }

    // One frame's synthetic g-buffer and lit colour, a value per pixel in row-major order.
    struct SyntheticFrame
    {
        uvec2 Extent{0};
        std::vector<f32> Depth;
        std::vector<vec3> Normal;
        std::vector<u8> Perceptual;
        std::vector<vec3> Color;
    };

    // The hashed g-buffer: an upward normal everywhere, the roughness of each pixel's kind, and no
    // lit colour.
    SyntheticFrame HashedFrame(const uvec2 extent, std::vector<f32> depth)
    {
        SyntheticFrame frame{.Extent = extent, .Depth = std::move(depth)};
        const usize pixels = static_cast<usize>(extent.x) * extent.y;
        frame.Normal.assign(pixels, vec3(0.0f, 1.0f, 0.0f));
        frame.Color.assign(pixels, vec3(0.0f));
        frame.Perceptual.resize(pixels);
        for (u32 y = 0; y < extent.y; ++y)
        {
            for (u32 x = 0; x < extent.x; ++x)
            {
                frame.Perceptual[static_cast<usize>(y) * extent.x + x] =
                    PerceptualByKind[static_cast<usize>(KindAt(x, y))];
            }
        }
        return frame;
    }

    // A synthetic frame, an environment cube of one colour, and a chain to run over them.
    struct ChainHarness
    {
        static constexpr vec3 EnvironmentColor{0.25f, 0.5f, 1.0f};
        static constexpr f32 IblFactor = 2.0f;

        Context& Ctx;
        uvec2 Extent;
        Unique<SsrChain> Chain;
        Ref<ImageView> Depth;
        Ref<ImageView> Normal;
        Ref<ImageView> Material;
        Ref<ImageView> Color;
        TextureHandle DepthHandle;
        TextureHandle NormalHandle;
        TextureHandle MaterialHandle;
        TextureHandle ColorHandle;

        ChainHarness(Context& context, AssetManager& assets, const uvec2 extent,
                     const std::vector<f32>& depth)
            : ChainHarness(context, assets, HashedFrame(extent, depth))
        {
        }

        ChainHarness(Context& context, AssetManager& assets, const SyntheticFrame& frame)
            : Ctx(context), Extent(frame.Extent)
        {
            const usize pixels = static_cast<usize>(Extent.x) * Extent.y;
            std::vector<u16> normals(pixels * 4);
            std::vector<u8> material(pixels * 4);
            std::vector<u16> color(pixels * 4);
            for (usize i = 0; i < pixels; ++i)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    normals[i * 4 + c] = glm::packHalf1x16(frame.Normal[i][c]);
                    color[i * 4 + c] = glm::packHalf1x16(frame.Color[i][c]);
                }
                normals[i * 4 + 3] = glm::packHalf1x16(0.0f);
                color[i * 4 + 3] = glm::packHalf1x16(1.0f);
                material[i * 4 + 0] = 255;
                material[i * 4 + 1] = frame.Perceptual[i];
                material[i * 4 + 2] = 0;
                material[i * 4 + 3] = 0;
            }
            Depth =
                Upload("SSR Test Depth", Format::R32Sfloat, std::as_bytes(std::span(frame.Depth)));
            Normal =
                Upload("SSR Test Normal", Format::RGBA16Sfloat, std::as_bytes(std::span(normals)));
            Material =
                Upload("SSR Test Material", Format::RGBA8Unorm, std::as_bytes(std::span(material)));
            Color = Upload("SSR Test Color", Format::RGBA16Sfloat, std::as_bytes(std::span(color)));
            BindlessRegistry& bindless = context.GetBindlessRegistry();
            DepthHandle = bindless.Register(Depth);
            NormalHandle = bindless.Register(Normal);
            MaterialHandle = bindless.Register(Material);
            ColorHandle = bindless.Register(Color);

            const Ref<Image> cube =
                Image::Create(context, {
                                           .Name = "SSR Test Environment",
                                           .Extent = {8, 8, 1},
                                           .Layers = 6,
                                           .Format = Format::RGBA16Sfloat,
                                           .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                       });
            std::vector<u16> texels(8 * 8 * 6 * 4);
            for (usize i = 0; i < texels.size(); i += 4)
            {
                texels[i + 0] = glm::packHalf1x16(EnvironmentColor.r);
                texels[i + 1] = glm::packHalf1x16(EnvironmentColor.g);
                texels[i + 2] = glm::packHalf1x16(EnvironmentColor.b);
                texels[i + 3] = glm::packHalf1x16(1.0f);
            }
            cube->UploadSync(
                std::span(reinterpret_cast<const u8*>(texels.data()), texels.size() * sizeof(u16)));

            Chain = SsrChain::Create(context, assets);
            Chain->Recreate(Extent);
            Chain->SetEnvironment(ImageView::Create(context, {
                                                                 .Name = "SSR Test Environment",
                                                                 .Image = cube,
                                                                 .ViewType = ImageViewType::Cube,
                                                                 .ArrayLayers = 6,
                                                             }));
        }

        ~ChainHarness()
        {
            BindlessRegistry& bindless = Ctx.GetBindlessRegistry();
            bindless.Release(DepthHandle);
            bindless.Release(NormalHandle);
            bindless.Release(MaterialHandle);
            bindless.Release(ColorHandle);
        }

        ChainHarness(const ChainHarness&) = delete;
        ChainHarness& operator=(const ChainHarness&) = delete;

        Ref<ImageView> Upload(const char* name, const Format format,
                              const std::span<const std::byte> bytes) const
        {
            const Ref<Image> image =
                Image::Create(Ctx, {
                                       .Name = name,
                                       .Extent = {Extent.x, Extent.y, 1},
                                       .Format = format,
                                       .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                   });
            image->UploadSync(std::span(reinterpret_cast<const u8*>(bytes.data()), bytes.size()));
            return ImageView::Create(Ctx, {.Name = name, .Image = image});
        }

        // Runs one frame of the chain: the constants, the passes, the frame count.
        void RunFrame(const SsrFrameInputs& inputs) const
        {
            Chain->PrepareFrame(inputs);
            Ctx.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    RenderGraph graph(Ctx);
                    const ResourceId depthId = graph.Import("Depth");
                    const ResourceId normalId = graph.Import("Normal");
                    const ResourceId materialId = graph.Import("Material");
                    const ResourceId colorId = graph.Import("Color");
                    Chain->Import(graph);
                    Chain->Declare(graph, SsrGraphInputs{
                                              .Depth = depthId,
                                              .Normal = normalId,
                                              .Material = materialId,
                                              .DepthHandle = DepthHandle,
                                              .NormalHandle = NormalHandle,
                                              .MaterialHandle = MaterialHandle,
                                              .Color = colorId,
                                              .ColorHandle = ColorHandle,
                                          });
                    std::vector<RenderGraph::ImportBinding> bindings{
                        {.Id = depthId, .View = Depth},
                        {.Id = normalId, .View = Normal},
                        {.Id = materialId, .View = Material},
                        {.Id = colorId, .View = Color}};
                    Chain->AppendImportBindings(bindings);
                    graph.Compile()->Execute(cmd, bindings);
                });
            Chain->AdvanceFrame();
        }

        [[nodiscard]] SsrFrameInputs Inputs() const
        {
            return SsrFrameInputs{
                .View = glm::lookAt(vec3(0.0f, 2.0f, 4.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f)),
                .Projection = glm::perspective(glm::radians(60.0f),
                                               static_cast<f32>(Extent.x) / Extent.y, 0.1f, 100.0f),
                .PrevViewProjection = mat4(1.0f),
                .RenderSize = Extent,
                .IblFactor = IblFactor,
            };
        }
    };

    // A floor at y = 0 and a small lit sphere standing above it to the camera's left, seen from
    // above and in front, with background past the floor's far edge. Every texel is the ray-traced
    // surface at its pixel centre, through the engine's own projection.
    struct FloorScene
    {
        static constexpr uvec2 Extent{128, 96};
        static constexpr vec3 SphereCenter{-1.0f, 0.45f, 0.0f};
        static constexpr f32 SphereRadius = 0.35f;
        static constexpr f32 FloorFar = 12.0f;
        static constexpr vec3 SphereColor{6.0f, 0.25f, 0.25f};
        static constexpr vec3 FloorColor{0.05f, 0.05f, 0.05f};

        CameraView Camera;
        SyntheticFrame Frame;
        // Floor pixels whose mirror ray strikes the sphere.
        std::vector<usize> Reflecting;
        // Floor pixels right of the camera, whose mirror ray runs away from the sphere and leaves
        // the screen over floor and background.
        std::vector<usize> Escaping;

        explicit FloorScene(const u8 floorPerceptual)
        {
            Camera.SetPerspective(glm::radians(60.0f),
                                  static_cast<f32>(Extent.x) / static_cast<f32>(Extent.y), 0.1f,
                                  100.0f);
            const vec3 eye{0.0f, 1.5f, 4.0f};
            Camera.SetView(eye, vec3(0.0f, 0.3f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
            const mat4 viewProjection = Camera.ViewProjection();
            const mat4 inverse = glm::inverse(viewProjection);

            const usize pixels = static_cast<usize>(Extent.x) * Extent.y;
            Frame = SyntheticFrame{.Extent = Extent,
                                   .Depth = std::vector<f32>(pixels, 0.0f),
                                   .Normal = std::vector<vec3>(pixels, vec3(0.0f)),
                                   .Perceptual = std::vector<u8>(pixels, 0),
                                   .Color = std::vector<vec3>(pixels, vec3(0.0f))};
            for (u32 y = 0; y < Extent.y; ++y)
            {
                for (u32 x = 0; x < Extent.x; ++x)
                {
                    const usize i = static_cast<usize>(y) * Extent.x + x;
                    // The engine's NDC runs y-down like UV.
                    const vec2 ndc = (vec2(x, y) + 0.5f) / vec2(Extent) * 2.0f - 1.0f;
                    const vec4 far = inverse * vec4(ndc, 0.5f, 1.0f);
                    const vec3 direction = glm::normalize(vec3(far) / far.w - eye);

                    const f32 sphereT = SphereHit(eye, direction);
                    const f32 floorT = direction.y < 0.0f ? -eye.y / direction.y : -1.0f;
                    const vec3 floorPoint = eye + floorT * direction;
                    const bool onFloor = floorT > 0.0f && -floorPoint.z < FloorFar;
                    vec3 point{0.0f};
                    if (sphereT > 0.0f && (!onFloor || sphereT < floorT))
                    {
                        point = eye + sphereT * direction;
                        Frame.Normal[i] = glm::normalize(point - SphereCenter);
                        Frame.Perceptual[i] = 204;
                        Frame.Color[i] = SphereColor;
                    }
                    else if (onFloor)
                    {
                        point = floorPoint;
                        Frame.Normal[i] = vec3(0.0f, 1.0f, 0.0f);
                        Frame.Perceptual[i] = floorPerceptual;
                        Frame.Color[i] = FloorColor;
                        const vec3 mirrored{direction.x, -direction.y, direction.z};
                        if (SphereHit(point, mirrored) > 0.0f)
                        {
                            Reflecting.push_back(i);
                        }
                        else if (point.x > 0.5f)
                        {
                            Escaping.push_back(i);
                        }
                    }
                    else
                    {
                        continue;
                    }
                    const vec4 clip = viewProjection * vec4(point, 1.0f);
                    Frame.Depth[i] = clip.z / clip.w;
                }
            }
        }

        // The nearest positive distance along a unit ray to the sphere, or -1 for a miss.
        static f32 SphereHit(const vec3 origin, const vec3 direction)
        {
            const vec3 offset = origin - SphereCenter;
            const f32 b = glm::dot(offset, direction);
            const f32 c = glm::dot(offset, offset) - SphereRadius * SphereRadius;
            const f32 discriminant = b * b - c;
            if (discriminant < 0.0f)
            {
                return -1.0f;
            }
            const f32 t = -b - std::sqrt(discriminant);
            return t > 1e-4f ? t : -1.0f;
        }

        [[nodiscard]] SsrFrameInputs Inputs() const
        {
            return SsrFrameInputs{
                .View = Camera.View(),
                .Projection = Camera.Projection(),
                .PrevViewProjection = Camera.ViewProjection(),
                .RenderSize = Extent,
                .IblFactor = ChainHarness::IblFactor,
            };
        }
    };

    // The radiance target a frame wrote, as RGBA halves widened to floats.
    std::vector<vec4> DownloadRadiance(const SsrChain& chain, const u32 index)
    {
        const std::vector<u8> bytes = chain.GetRadianceView(index)->GetImage()->Download();
        const auto* halves = reinterpret_cast<const u16*>(bytes.data());
        std::vector<vec4> texels(bytes.size() / (4 * sizeof(u16)));
        for (usize i = 0; i < texels.size(); ++i)
        {
            texels[i] = vec4(
                glm::unpackHalf1x16(halves[i * 4 + 0]), glm::unpackHalf1x16(halves[i * 4 + 1]),
                glm::unpackHalf1x16(halves[i * 4 + 2]), glm::unpackHalf1x16(halves[i * 4 + 3]));
        }
        return texels;
    }

    bool FiniteAndNonNegative(const std::vector<vec4>& texels)
    {
        return std::ranges::all_of(texels,
                                   [](const vec4 texel)
                                   {
                                       return std::isfinite(texel.r) && std::isfinite(texel.g) &&
                                              std::isfinite(texel.b) && std::isfinite(texel.a) &&
                                              glm::all(glm::greaterThanEqual(texel, vec4(0.0f)));
                                   });
    }

    std::vector<f32> SyntheticDepth(const uvec2 extent)
    {
        std::vector<f32> depth(static_cast<usize>(extent.x) * extent.y);
        for (u32 y = 0; y < extent.y; ++y)
        {
            for (u32 x = 0; x < extent.x; ++x)
            {
                depth[static_cast<usize>(y) * extent.x + x] =
                    KindAt(x, y) == Kind::Background ? 0.0f : 0.5f;
            }
        }
        return depth;
    }
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "ssr chain: every depth-hierarchy texel is the nearest of its 2x2 children")
{
    AssetManager assets(Context, Tasks, Types);

    for (const uvec2 extent : {uvec2(37, 23), uvec2(16, 9)})
    {
        CAPTURE(extent.x);
        CAPTURE(extent.y);
        // A varied reverse-Z field with background (0) scattered through it.
        std::vector<f32> depth(static_cast<usize>(extent.x) * extent.y);
        for (usize i = 0; i < depth.size(); ++i)
        {
            const u32 h = Hash(static_cast<u32>(i), 17u);
            depth[i] = (h % 5u) == 0u ? 0.0f : static_cast<f32>(h % 10007u) / 10007.0f;
        }

        const ChainHarness harness(Context, assets, extent, depth);
        harness.RunFrame(harness.Inputs());
        const SsrChain& chain = *harness.Chain;

        // The reference's level count: min(7, ceil(log2(longest side))).
        const u32 longest = std::max(extent.x, extent.y);
        const u32 expectedLevels =
            std::min(7u, static_cast<u32>(std::ceil(std::log2(static_cast<f64>(longest)))));
        REQUIRE(chain.GetDepthHierarchyLevels() == expectedLevels);

        std::vector<f32> parent = depth;
        uvec2 parentExtent = extent;
        u32 mismatches = 0;
        for (u32 level = 0; level < chain.GetDepthHierarchyLevels(); ++level)
        {
            const uvec2 levelExtent{std::max(extent.x >> level, 1u),
                                    std::max(extent.y >> level, 1u)};
            const std::vector<f32> got =
                DownloadLevel(Context, chain.GetDepthHierarchy(),
                              chain.GetDepthHierarchyLevel(level), level, levelExtent);
            std::vector<f32> expected(got.size());
            for (u32 y = 0; y < levelExtent.y; ++y)
            {
                for (u32 x = 0; x < levelExtent.x; ++x)
                {
                    f32 value = 0.0f;
                    if (level == 0)
                    {
                        value = parent[static_cast<usize>(y) * parentExtent.x + x];
                    }
                    else
                    {
                        for (u32 j = 0; j < 2; ++j)
                        {
                            for (u32 i = 0; i < 2; ++i)
                            {
                                const u32 cx = 2 * x + i;
                                const u32 cy = 2 * y + j;
                                if (cx < parentExtent.x && cy < parentExtent.y)
                                {
                                    value = std::max(
                                        value,
                                        parent[static_cast<usize>(cy) * parentExtent.x + cx]);
                                }
                            }
                        }
                    }
                    expected[static_cast<usize>(y) * levelExtent.x + x] = value;
                }
            }
            for (usize i = 0; i < got.size(); ++i)
            {
                mismatches += got[i] == expected[i] ? 0u : 1u;
            }
            parent = got;
            parentExtent = levelExtent;
        }
        CHECK(mismatches == 0);
    }
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "ssr chain: the classifier lists exactly the pixels and tiles the reference "
                  "rule selects")
{
    AssetManager assets(Context, Tasks, Types);
    // Neither side a multiple of the 8-pixel tile, so the last row and column of tiles are
    // partial.
    constexpr uvec2 Extent{36, 20};
    const ChainHarness harness(Context, assets, Extent, SyntheticDepth(Extent));
    const SsrChain& chain = *harness.Chain;

    // Variance history on a third of the pixels, in both targets so either parity reads it.
    {
        std::vector<u16> variance(static_cast<usize>(Extent.x) * Extent.y);
        for (u32 y = 0; y < Extent.y; ++y)
        {
            for (u32 x = 0; x < Extent.x; ++x)
            {
                variance[static_cast<usize>(y) * Extent.x + x] =
                    glm::packHalf1x16(HasVariance(x, y) ? 1.0f : 0.0f);
            }
        }
        for (u32 i = 0; i < 2; ++i)
        {
            chain.GetVarianceView(i)->GetImage()->UploadSync(std::span(
                reinterpret_cast<const u8*>(variance.data()), variance.size() * sizeof(u16)));
        }
    }

    struct Case
    {
        u32 SamplesPerQuad;
        bool Guided;
        f32 VarianceThreshold;
    };
    for (const Case c : {
             Case{.SamplesPerQuad = 1, .Guided = false, .VarianceThreshold = 0.0f},
             Case{.SamplesPerQuad = 2, .Guided = false, .VarianceThreshold = 0.0f},
             Case{.SamplesPerQuad = 4, .Guided = false, .VarianceThreshold = 0.0f},
             Case{.SamplesPerQuad = 1, .Guided = true, .VarianceThreshold = 0.5f},
             Case{.SamplesPerQuad = 2, .Guided = true, .VarianceThreshold = 0.5f},
         })
    {
        CAPTURE(c.SamplesPerQuad);
        CAPTURE(c.Guided);
        SsrFrameInputs inputs = harness.Inputs();
        inputs.SamplesPerQuad = c.SamplesPerQuad;
        inputs.VarianceGuidedTracing = c.Guided;
        inputs.VarianceThreshold = c.VarianceThreshold;
        const u32 radianceIndex = chain.GetRadianceIndex();
        harness.RunFrame(inputs);

        const ExpectedClassification expected =
            Classify(Extent, c.SamplesPerQuad, c.Guided, c.VarianceThreshold);
        // The synthetic g-buffer exercises the rule: some rays, some of them copying to a quad
        // neighbour wherever a quad shares its rays, and a tile left out.
        const auto copies =
            std::ranges::count_if(expected.Rays, [](const u32 ray) { return (ray >> 29) != 0u; });
        REQUIRE_FALSE(expected.Rays.empty());
        REQUIRE((copies > 0) == (c.SamplesPerQuad != 4));
        REQUIRE(expected.Tiles.size() < 15);
        const std::vector<u32> counters = Words(chain.GetRayCounter()->Download());
        const std::vector<u32> args = Words(chain.GetIndirectArgs()->Download());
        const u32 rays = counters[1];
        const u32 tiles = counters[3];

        // The counters rotated and the arguments are the reference formula of the counts.
        CHECK(counters[0] == 0);
        CHECK(counters[2] == 0);
        CHECK(rays == expected.Rays.size());
        CHECK(tiles == expected.Tiles.size());
        CHECK(args == std::vector<u32>{(rays + 63) / 64, 1, 1, tiles, 1, 1});

        std::vector<u32> rayList = Words(chain.GetRayList()->Download());
        rayList.resize(std::min<usize>(rays, rayList.size()));
        std::ranges::sort(rayList);
        CHECK(rayList == expected.Rays);

        std::vector<u32> tileList = Words(chain.GetDenoiserTileList()->Download());
        tileList.resize(std::min<usize>(tiles, tileList.size()));
        std::ranges::sort(tileList);
        CHECK(tileList == expected.Tiles);

        // A rough reflective pixel holds the environment times the IBL factor and a background
        // pixel zero; a glossy one is the intersection's, which the cases below check. The
        // roughness the classifier read is stored in alpha.
        const std::vector<u8> radiance =
            chain.GetRadianceView(radianceIndex)->GetImage()->Download();
        const std::vector<u8> roughness = chain.GetExtractedRoughnessView()->GetImage()->Download();
        const auto* halves = reinterpret_cast<const u16*>(radiance.data());
        f32 radianceError = 0.0f;
        f32 roughnessError = 0.0f;
        bool finite = true;
        for (u32 y = 0; y < Extent.y; ++y)
        {
            for (u32 x = 0; x < Extent.x; ++x)
            {
                const usize i = static_cast<usize>(y) * Extent.x + x;
                const Kind kind = KindAt(x, y);
                const bool classified = kind == Kind::Rough || kind == Kind::Background;
                const vec3 expectedRadiance =
                    kind == Kind::Rough ? ChainHarness::EnvironmentColor * ChainHarness::IblFactor
                                        : vec3(0.0f);
                for (u32 channel = 0; channel < 3; ++channel)
                {
                    const f32 value = glm::unpackHalf1x16(halves[i * 4 + channel]);
                    finite = finite && std::isfinite(value) && value >= 0.0f;
                    if (classified)
                    {
                        radianceError =
                            std::max(radianceError, std::abs(value - expectedRadiance[channel]));
                    }
                }
                roughnessError =
                    std::max(roughnessError, std::abs(roughness[i] / 255.0f - AlphaOf(kind)));
            }
        }
        CHECK(finite);
        CHECK(radianceError < 1e-3f);
        CHECK(roughnessError <= 0.5f / 255.0f + 1e-6f);
    }
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "ssr chain: the blue-noise tile is the sampler's formula over the cooked tables")
{
    AssetManager assets(Context, Tasks, Types);
    constexpr uvec2 Extent{16, 16};
    ChainHarness harness(Context, assets, Extent, SyntheticDepth(Extent));
    // A frame index past zero, so the golden-ratio offset is exercised.
    harness.RunFrame(harness.Inputs());
    harness.RunFrame(harness.Inputs());
    harness.RunFrame(harness.Inputs());
    const u32 frame = 2;

    const AssetResult<AssetHandle<RawAsset>> tables = assets.LoadSync<RawAsset>(BlueNoiseTablesId);
    REQUIRE(tables.has_value());
    const std::vector<u8>& bytes = tables.value().Get()->Bytes;
    constexpr usize SobolCount = 256 * 256;
    REQUIRE(bytes.size() == SobolCount + 128 * 128 * 8);

    const auto Sample = [&](const u32 i, const u32 j, const u32 dimension)
    {
        const u32 value = bytes[dimension];
        const u32 originalIndex = (dimension % 8u) + (i + j * 128u) * 8u;
        const u32 scrambled = value ^ bytes[SobolCount + originalIndex];
        const f32 sample = (static_cast<f32>(scrambled) + 0.5f) / 256.0f;
        return std::fmod(sample + static_cast<f32>(frame & 0xFFu) * 1.61803398875f, 1.0f);
    };

    const std::vector<u8> noise = harness.Chain->GetBlueNoiseView()->GetImage()->Download();
    REQUIRE(noise.size() == SsrChain::BlueNoiseSize * SsrChain::BlueNoiseSize * 2);
    f32 worst = 0.0f;
    f32 lowest = 1.0f;
    f32 highest = 0.0f;
    for (u32 j = 0; j < SsrChain::BlueNoiseSize; ++j)
    {
        for (u32 i = 0; i < SsrChain::BlueNoiseSize; ++i)
        {
            for (u32 dimension = 0; dimension < 2; ++dimension)
            {
                const f32 got =
                    noise[(static_cast<usize>(j) * SsrChain::BlueNoiseSize + i) * 2 + dimension] /
                    255.0f;
                lowest = std::min(lowest, got);
                highest = std::max(highest, got);
                // Circular distance: a value a rounding step below 1 wraps to 0 either way.
                const f32 difference = std::abs(got - Sample(i, j, dimension));
                worst = std::max(worst, std::min(difference, 1.0f - difference));
            }
        }
    }
    CHECK(lowest >= 0.0f);
    CHECK(highest <= 1.0f);
    CHECK(worst <= 1.0f / 255.0f + 1e-4f);
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "ssr chain: a mirror floor reflects a small sphere and falls back to the "
                  "environment off screen")
{
    AssetManager assets(Context, Tasks, Types);
    const FloorScene scene(0);
    REQUIRE(scene.Reflecting.size() >= 8);
    REQUIRE(scene.Escaping.size() >= 8);
    const ChainHarness harness(Context, assets, scene.Frame);
    const u32 radianceIndex = harness.Chain->GetRadianceIndex();
    harness.RunFrame(scene.Inputs());
    const std::vector<vec4> radiance = DownloadRadiance(*harness.Chain, radianceIndex);

    CHECK(FiniteAndNonNegative(radiance));

    // A ray that leaves the screen has confidence 0, so it holds the environment along it times
    // the IBL factor, exactly; the cube is one colour, so that is the same for every direction.
    const vec3 environment = ChainHarness::EnvironmentColor * ChainHarness::IblFactor;
    usize notEnvironment = 0;
    for (const usize i : scene.Escaping)
    {
        notEnvironment += vec3(radiance[i]) == environment ? 0u : 1u;
    }
    CHECK(notEnvironment == 0);

    // The sphere is in range of the floor, so some of the floor under it reads its colour back.
    const usize reflected = std::ranges::count_if(scene.Reflecting, [&](const usize i)
                                                  { return radiance[i].r > environment.r + 0.1f; });
    CHECK(reflected > 0);
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "ssr chain: every quad neighbour a ray copies to holds that ray's value")
{
    AssetManager assets(Context, Tasks, Types);
    // Perceptual 0.3 (alpha 0.09): glossy, under the threshold, and not a mirror, so one ray per
    // quad is traced and copied to the other three.
    const FloorScene scene(77);
    const ChainHarness harness(Context, assets, scene.Frame);
    const u32 radianceIndex = harness.Chain->GetRadianceIndex();
    SsrFrameInputs inputs = scene.Inputs();
    inputs.SamplesPerQuad = 1;
    harness.RunFrame(inputs);
    const std::vector<vec4> radiance = DownloadRadiance(*harness.Chain, radianceIndex);
    CHECK(FiniteAndNonNegative(radiance));

    const u32 rays = Words(harness.Chain->GetRayCounter()->Download())[1];
    std::vector<u32> rayList = Words(harness.Chain->GetRayList()->Download());
    rayList.resize(std::min<usize>(rays, rayList.size()));
    const auto At = [&](const u32 x, const u32 y)
    { return radiance[static_cast<usize>(y) * FloorScene::Extent.x + x]; };
    usize copies = 0;
    usize mismatches = 0;
    for (const u32 ray : rayList)
    {
        const u32 x = ray & 0x7FFFu;
        const u32 y = (ray >> 15) & 0x3FFFu;
        const std::array<std::pair<bool, uvec2>, 3> targets{
            std::pair{((ray >> 29) & 1u) != 0u, uvec2(x ^ 1u, y)},
            std::pair{((ray >> 30) & 1u) != 0u, uvec2(x, y ^ 1u)},
            std::pair{((ray >> 31) & 1u) != 0u, uvec2(x ^ 1u, y ^ 1u)},
        };
        for (const auto& [copied, target] : targets)
        {
            if (copied)
            {
                ++copies;
                mismatches += At(target.x, target.y) == At(x, y) ? 0u : 1u;
            }
        }
    }
    REQUIRE(copies > 0);
    CHECK(mismatches == 0);
}

TEST_CASE_FIXTURE(
    Test::GpuFixture,
    "ssr chain: a renderer records the chain's passes with SSR on and none with it off")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const Unique<Scene> scene = Scene::Create(Types);
    CameraView camera;
    camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, -5.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    constexpr std::array<string_view, 5> Passes{"SSR Depth Downsample", "SSR Classify Tiles",
                                                "SSR Prepare Blue Noise",
                                                "SSR Prepare Indirect Args", "SSR Intersect"};
    struct Case
    {
        bool Ssr;
        AntiAliasingMode AntiAliasing;
    };
    for (const Case c : {Case{.Ssr = false, .AntiAliasing = AntiAliasingMode::None},
                         Case{.Ssr = true, .AntiAliasing = AntiAliasingMode::None},
                         Case{.Ssr = true, .AntiAliasing = AntiAliasingMode::TAA}})
    {
        const bool ssr = c.Ssr;
        CAPTURE(ssr);
        CAPTURE(static_cast<u32>(c.AntiAliasing));
        SceneRendererSettings settings;
        settings.Shadows = false;
        settings.AO = false;
        settings.SSR = ssr;
        settings.AntiAliasing = c.AntiAliasing;
        const Unique<SceneRenderer> renderer = SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = {32, 32},
            .Settings = settings,
        });
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd,
                                  Veng::Renderer::SceneView{.World = *scene, .Camera = camera});
            });
        for (const string_view pass : Passes)
        {
            CAPTURE(pass);
            CHECK(renderer->DidRecordPassLastFrame(pass) == ssr);
        }
        const Ref<ImageView> radiance = renderer->GetSsrRadianceView();
        REQUIRE((radiance != nullptr) == ssr);
        if (radiance != nullptr)
        {
            const std::vector<u8> bytes = radiance->GetImage()->Download();
            const auto* halves = reinterpret_cast<const u16*>(bytes.data());
            bool finite = true;
            for (usize i = 0; i < bytes.size() / sizeof(u16); ++i)
            {
                const f32 value = glm::unpackHalf1x16(halves[i]);
                finite = finite && std::isfinite(value) && value >= 0.0f;
            }
            CHECK(finite);
        }
    }
}
