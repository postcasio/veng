// The flipbook shading model's binding-free core, evaluated by the shader that ships it: the
// six-way weights, the temperature decode and the emission ramp with its ignition gate, the frame
// blend with and without motion, the soft-edge fade, and the view-depth solve a soft edge measures
// its gap with. One one-workgroup dispatch per function over cases the test uploads, so each
// property is read off the shader's own numbers rather than off a rendered scene.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <array>
#include <cmath>
#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetBuild.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

// glm's headers after Veng.h, which configures glm first.
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The test shader pack's sprite_shading.comp id.
    constexpr AssetId SpriteShadingCompId{0x959CE75A45092DCEULL};

    // The harness's modes, matching sprite_shading.comp.slang.
    enum class Mode : u32
    {
        Weights = 0,
        Temperature = 1,
        Ramp = 2,
        FrameBlend = 3,
        SoftFade = 4,
        ViewDepth = 5,
    };

    struct SpriteShadingPush
    {
        u32 Mode;
        u32 CaseCount;
        f32 Fraction;
        u32 Pad;
    };

    // Matches SpriteEmissionRamp.
    struct Ramp
    {
        std::array<vec4, 8> Keys{};
        vec4 Params{0.0f};
    };

    // Two 16×16 frames side by side, the atlas the frame-blend cases read.
    constexpr u32 FrameSize = 16;

    // A Gaussian blob of unit peak centred on texel (cx, 7.5) of a frame.
    f32 Blob(const f32 x, const f32 y, const f32 cx)
    {
        const f32 dx = x - cx;
        const f32 dy = y - 7.5f;
        return std::exp(-(dx * dx + dy * dy) / (2.0f * 1.5f * 1.5f));
    }

    Ref<Texture> HalfFloatTexture(Context& context, const char* name, const vector<vec4>& texels)
    {
        vector<u16> halves(texels.size() * 4);
        for (usize i = 0; i < texels.size(); ++i)
        {
            for (usize c = 0; c < 4; ++c)
            {
                halves[i * 4 + c] = glm::packHalf1x16(texels[i][static_cast<int>(c)]);
            }
        }
        return Veng::Detail::BuildAssetSync(
            context, TextureData{
                         .Name = name,
                         .Extent = {FrameSize * 2, FrameSize},
                         .Format = Format::RGBA16Sfloat,
                         .Pixels = std::span(reinterpret_cast<const u8*>(halves.data()),
                                             halves.size() * sizeof(u16)),
                         .Sampler = {.AddressModeU = AddressMode::ClampToEdge,
                                     .AddressModeV = AddressMode::ClampToEdge,
                                     .AddressModeW = AddressMode::ClampToEdge},
                     });
    }

    struct Harness
    {
        Context& Ctx;
        AssetHandle<Veng::Shader> Shader;
        Ref<DescriptorSetLayout> SetLayout;
        Ref<PipelineLayout> Layout;
        Ref<ComputePipeline> Pipeline;
        Ref<Texture> Atlas;
        Ref<Texture> Motion;
        Ref<Texture> Still;
        Ref<Buffer> RampBuffer;

        vector<vec4> Run(const Mode mode, const vector<vec4>& cases, const u32 caseCount,
                         const u32 resultsPerCase, const f32 fraction = 0.0f,
                         const bool moving = true) const
        {
            const Ref<Buffer> caseBuffer =
                Buffer::Create(Ctx, {
                                        .Name = "Sprite Shading Cases",
                                        .Size = cases.size() * sizeof(vec4),
                                        .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                    });
            caseBuffer->UploadSync(
                std::span(reinterpret_cast<const u8*>(cases.data()), cases.size() * sizeof(vec4)));
            const u64 resultBytes = static_cast<u64>(caseCount) * resultsPerCase * sizeof(vec4);
            const Ref<Buffer> resultBuffer =
                Buffer::Create(Ctx, {
                                        .Name = "Sprite Shading Results",
                                        .Size = resultBytes,
                                        .Usage = BufferUsage::Storage | BufferUsage::TransferSrc,
                                    });

            const Ref<DescriptorSet> set =
                DescriptorSet::Create(Ctx, {.Name = "Sprite Shading Set", .Layout = SetLayout});
            set->Write(0, caseBuffer);
            set->Write(1, resultBuffer);
            set->Write(2, Atlas->GetView());
            set->Write(3, moving ? Motion->GetView() : Still->GetView());
            set->Write(4, Atlas->GetSampler());
            set->Write(5, RampBuffer);

            Ctx.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    cmd.BindPipeline(Pipeline);
                    cmd.BindDescriptorSets({
                        .Sets = {set},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(SpriteShadingPush{.Mode = static_cast<u32>(mode),
                                                        .CaseCount = caseCount,
                                                        .Fraction = fraction});
                    cmd.Dispatch(caseCount, 1, 1);
                });

            const vector<u8> downloaded = resultBuffer->Download();
            REQUIRE(downloaded.size() == resultBytes);
            vector<vec4> results(static_cast<usize>(caseCount) * resultsPerCase);
            std::memcpy(results.data(), downloaded.data(), downloaded.size());
            return results;
        }
    };

    Harness MakeHarness(Veng::Test::GpuFixture& gpu, AssetManager& assets, const Ramp& ramp)
    {
        const auto shader = assets.LoadSync<Veng::Shader>(SpriteShadingCompId);
        REQUIRE_MESSAGE(shader, shader.error().Detail);

        const auto binding = [](const u32 index, const DescriptorType type)
        {
            return DescriptorBinding{
                .Binding = index, .Type = type, .Count = 1, .Stages = ShaderStage::Compute};
        };
        Harness harness{.Ctx = gpu.Context, .Shader = *shader};
        harness.SetLayout = DescriptorSetLayout::Create(
            gpu.Context, {
                             .Name = "Sprite Shading Set Layout",
                             .Bindings =
                                 {
                                     binding(0, DescriptorType::StorageBuffer),
                                     binding(1, DescriptorType::StorageBuffer),
                                     binding(2, DescriptorType::SampledImage),
                                     binding(3, DescriptorType::SampledImage),
                                     binding(4, DescriptorType::Sampler),
                                     binding(5, DescriptorType::StorageBuffer),
                                 },
                         });
        harness.Layout = PipelineLayout::Create(
            gpu.Context, {
                             .Name = "Sprite Shading Layout",
                             .DescriptorSetLayouts = {harness.SetLayout},
                             .PushConstantRanges = {PushConstantRange::Of<SpriteShadingPush>(
                                 ShaderStage::Compute)},
                         });
        harness.Pipeline = ComputePipeline::Create(
            gpu.Context, {
                             .Name = "Sprite Shading Pipeline",
                             .PipelineLayout = harness.Layout,
                             .ShaderStage = {.Stage = ShaderStage::Compute,
                                             .Module = harness.Shader.Get()->Module},
                         });

        // Frame N holds the blob at texel x = 5, frame N+1 the same blob at x = 9. Every texel of
        // the motion sheet encodes +4 texels (a quarter of the frame) along u and none along v,
        // under a decode scale of 0.25: a stored 1 is the full scale, a stored 128/255 zero.
        vector<vec4> atlas(static_cast<usize>(FrameSize) * 2 * FrameSize);
        const vector<vec4> motion(atlas.size(), vec4(1.0f, 128.0f / 255.0f, 0.0f, 0.0f));
        const vector<vec4> still(atlas.size(), vec4(128.0f / 255.0f, 128.0f / 255.0f, 0.0f, 0.0f));
        for (u32 y = 0; y < FrameSize; ++y)
        {
            for (u32 x = 0; x < FrameSize; ++x)
            {
                const f32 px = static_cast<f32>(x) + 0.5f;
                const f32 py = static_cast<f32>(y) + 0.5f;
                const usize row = static_cast<usize>(y) * FrameSize * 2;
                atlas[row + x] = vec4(Blob(px, py, 5.5f));
                atlas[row + FrameSize + x] = vec4(Blob(px, py, 9.5f));
            }
        }
        harness.Atlas = HalfFloatTexture(gpu.Context, "Sprite Shading Atlas", atlas);
        harness.Motion = HalfFloatTexture(gpu.Context, "Sprite Shading Motion", motion);
        harness.Still = HalfFloatTexture(gpu.Context, "Sprite Shading Still", still);

        harness.RampBuffer = Buffer::Create(
            gpu.Context, {
                             .Name = "Sprite Shading Ramp",
                             .Size = sizeof(Ramp),
                             .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
                         });
        harness.RampBuffer->UploadSync(std::span(reinterpret_cast<const u8*>(&ramp), sizeof(Ramp)));
        return harness;
    }

    // The 16×16 grid of quad coordinates, texel centres, as frame-blend cases.
    vector<vec4> FrameGrid(const f32 motionScale)
    {
        vector<vec4> cases;
        for (u32 y = 0; y < FrameSize; ++y)
        {
            for (u32 x = 0; x < FrameSize; ++x)
            {
                cases.emplace_back((static_cast<f32>(x) + 0.5f) / FrameSize,
                                   (static_cast<f32>(y) + 0.5f) / FrameSize, motionScale, 0.0f);
            }
        }
        return cases;
    }

    // The stored texel of one frame, as the atlas holds it (through half precision).
    f32 Stored(const u32 x, const u32 y, const f32 cx)
    {
        return glm::unpackHalf1x16(
            glm::packHalf1x16(Blob(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f, cx)));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "The flipbook shading core weights, decodes, blends and fades as it promises")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mounted = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mounted, mounted.error());

    // Three keys, red at 0.2, green at 0.5, blue at 0.9; ignition at 0.4.
    Ramp ramp;
    ramp.Keys[0] = vec4(1.0f, 0.0f, 0.0f, 0.2f);
    ramp.Keys[1] = vec4(0.0f, 2.0f, 0.0f, 0.5f);
    ramp.Keys[2] = vec4(0.0f, 0.0f, 4.0f, 0.9f);
    ramp.Params = vec4(0.4f, 3.0f, 0.0f, 0.0f);
    const Harness harness = MakeHarness(*this, assets, ramp);

    SUBCASE("A light along an axis weights only that axis's map; any light's weights sum to 1")
    {
        // ±X, ±Y, ±Z, then twelve fixed off-axis directions.
        vector<vec4> cases{vec4(1, 0, 0, 0),  vec4(0, 1, 0, 0),  vec4(0, 0, -1, 0),
                           vec4(-1, 0, 0, 0), vec4(0, -1, 0, 0), vec4(0, 0, 1, 0)};
        for (u32 i = 0; i < 12; ++i)
        {
            const f32 a = 0.7f + static_cast<f32>(i) * 0.53f;
            const f32 b = 0.3f + static_cast<f32>(i) * 1.21f;
            cases.emplace_back(glm::normalize(vec3(std::cos(a) * std::sin(b),
                                                   std::sin(a) * std::sin(b), std::cos(b))),
                               0.0f);
        }
        const auto count = static_cast<u32>(cases.size());
        const vector<vec4> results = harness.Run(Mode::Weights, cases, count, 2);

        // Axis i (lightA's +X, +Y, -Z, then lightB's -X, -Y, +Z) weights exactly its own map.
        u32 axisMisses = 0;
        for (u32 axis = 0; axis < 6; ++axis)
        {
            const vec3 wA(results[2 * axis]);
            const vec3 wB(results[2 * axis + 1]);
            const std::array<f32, 6> w{wA.x, wA.y, wA.z, wB.x, wB.y, wB.z};
            for (u32 j = 0; j < 6; ++j)
            {
                axisMisses += w[j] != (j == axis ? 1.0f : 0.0f) ? 1u : 0u;
            }
        }
        CHECK(axisMisses == 0);

        f32 worstSum = 0.0f;
        f32 lowest = 1.0f;
        f32 highest = 0.0f;
        for (u32 i = 6; i < count; ++i)
        {
            worstSum = std::max(worstSum, std::abs(results[2 * i].w - 1.0f));
            for (const vec4& w : {results[2 * i], results[2 * i + 1]})
            {
                lowest = std::min({lowest, w.x, w.y, w.z});
                highest = std::max({highest, w.x, w.y, w.z});
            }
        }
        CHECK(worstSum < 1e-6f);
        CHECK(lowest >= 0.0f);
        CHECK(highest <= 1.0f);
    }

    SUBCASE("The temperature decode inverts the sqrt encoding")
    {
        vector<vec4> cases;
        for (u32 i = 0; i <= 10; ++i)
        {
            cases.emplace_back(std::sqrt(static_cast<f32>(i) / 10.0f), 0.0f, 0.0f, 0.0f);
        }
        const vector<vec4> results = harness.Run(Mode::Temperature, cases, 11, 1);
        f32 worst = 0.0f;
        for (u32 i = 0; i <= 10; ++i)
        {
            worst = std::max(worst, std::abs(results[i].x - static_cast<f32>(i) / 10.0f));
            worst = std::max(worst, std::abs(results[i].y - cases[i].x));
        }
        CHECK(worst < 1e-6f);
    }

    SUBCASE("The ramp passes through its keys, is linear between them, and holds its ends")
    {
        // t at each key, the midpoint between the first two, below the first and above the last.
        const vector<vec4> cases{vec4(0.2f, 0, 0, 0),  vec4(0.5f, 0, 0, 0), vec4(0.9f, 0, 0, 0),
                                 vec4(0.35f, 0, 0, 0), vec4(0.0f, 0, 0, 0), vec4(1.0f, 0, 0, 0)};
        const vector<vec4> results = harness.Run(Mode::Ramp, cases, 6, 1);
        const std::array<vec3, 6> expected{vec3(1, 0, 0),    vec3(0, 2, 0), vec3(0, 0, 4),
                                           vec3(0.5f, 1, 0), vec3(1, 0, 0), vec3(0, 0, 4)};
        f32 worst = 0.0f;
        for (usize i = 0; i < expected.size(); ++i)
        {
            const vec3 delta = glm::abs(vec3(results[i]) - expected[i]);
            worst = std::max({worst, delta.x, delta.y, delta.z});
        }
        CHECK(worst < 1e-5f);
    }

    SUBCASE("With no gate width, nothing below ignition emits and everything above it does")
    {
        vector<vec4> cases;
        for (u32 i = 0; i <= 40; ++i)
        {
            cases.emplace_back(static_cast<f32>(i) / 40.0f + 0.0031f, 0.0f, 0.0f, 0.0f);
        }
        const auto count = static_cast<u32>(cases.size());
        const vector<vec4> results = harness.Run(Mode::Ramp, cases, count, 1);
        u32 wrong = 0;
        for (u32 i = 0; i < count; ++i)
        {
            wrong += results[i].w != (cases[i].x >= 0.4f ? 1.0f : 0.0f) ? 1u : 0u;
        }
        CHECK(wrong == 0);
    }

    SUBCASE(
        "A frame blend's ends are the frames, and motion moves a feature instead of ghosting it")
    {
        const vector<vec4> grid = FrameGrid(0.25f);
        const auto count = static_cast<u32>(grid.size());
        const vector<vec4> atStart = harness.Run(Mode::FrameBlend, grid, count, 1, 0.0f);
        const vector<vec4> atEnd = harness.Run(Mode::FrameBlend, grid, count, 1, 1.0f);
        const vector<vec4> halfway = harness.Run(Mode::FrameBlend, grid, count, 1, 0.5f);

        f32 startError = 0.0f;
        f32 endError = 0.0f;
        f32 sum = 0.0f;
        f32 weighted = 0.0f;
        f32 peak = 0.0f;
        for (u32 y = 0; y < FrameSize; ++y)
        {
            for (u32 x = 0; x < FrameSize; ++x)
            {
                const u32 i = y * FrameSize + x;
                startError = std::max(startError, std::abs(atStart[i].x - Stored(x, y, 5.5f)));
                endError = std::max(endError, std::abs(atEnd[i].x - Stored(x, y, 9.5f)));
                sum += halfway[i].x;
                weighted += halfway[i].x * static_cast<f32>(x);
                peak = std::max(peak, halfway[i].x);
            }
        }
        CHECK(startError < 1e-4f);
        CHECK(endError < 1e-4f);
        // Halfway the blob sits halfway, at texel 7, at full strength: one blob, not two faded ones.
        CHECK(weighted / sum == doctest::Approx(7.0f).epsilon(0.25f / 7.0f));
        CHECK(peak >= 0.9f * Stored(5, 7, 5.5f));

        // With no motion the halfway blend is the plain crossfade of the two frames.
        const vector<vec4> crossfade =
            harness.Run(Mode::FrameBlend, FrameGrid(0.25f), count, 1, 0.5f, /*moving=*/false);
        f32 crossfadeError = 0.0f;
        for (u32 y = 0; y < FrameSize; ++y)
        {
            for (u32 x = 0; x < FrameSize; ++x)
            {
                const f32 expected = 0.5f * (Stored(x, y, 5.5f) + Stored(x, y, 9.5f));
                crossfadeError =
                    std::max(crossfadeError, std::abs(crossfade[y * FrameSize + x].x - expected));
            }
        }
        CHECK(crossfadeError < 1e-4f);
    }

    SUBCASE("The soft fade is monotone and bounded, and a distance of 0 never fades")
    {
        constexpr f32 Distance = 0.8f;
        vector<vec4> cases;
        for (u32 i = 0; i < 32; ++i)
        {
            const f32 gap = -Distance + static_cast<f32>(i) * (3.0f * Distance / 31.0f);
            cases.emplace_back(gap, Distance, 0.0f, 0.0f);
            cases.emplace_back(gap, 0.0f, 0.0f, 0.0f);
        }
        const vector<vec4> results = harness.Run(Mode::SoftFade, cases, 64, 1);
        u32 violations = 0;
        f32 previous = 0.0f;
        for (u32 i = 0; i < 32; ++i)
        {
            const f32 gap = cases[2 * i].x;
            const f32 fade = results[2 * i].x;
            violations += fade < previous ? 1u : 0u;
            violations += gap <= 0.0f && fade != 0.0f ? 1u : 0u;
            violations += gap >= Distance && fade != 1.0f ? 1u : 0u;
            violations += results[2 * i + 1].x != 1.0f ? 1u : 0u;
            previous = fade;
        }
        CHECK(violations == 0);
    }

    SUBCASE("A view depth survives the round trip through a reverse-Z device depth")
    {
        // A reverse-Z infinite perspective and a reverse-Z orthographic projection, by their depth
        // and w rows (A, B, C, D): device depth = (A·z + B) / (C·z + D) at view-space z.
        const glm::mat4 perspective =
            glm::infinitePerspectiveRH_ZO(glm::radians(50.0f), 1.0f, 0.1f);
        const auto reverse = [](const glm::mat4& projection)
        {
            // Reverse-Z maps device depth d to 1 - d.
            glm::mat4 flip(1.0f);
            flip[2][2] = -1.0f;
            flip[3][2] = 1.0f;
            return flip * projection;
        };
        const std::array<glm::mat4, 2> projections{
            reverse(perspective), reverse(glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 200.0f))};
        const std::array<f32, 5> depths{0.5f, 2.0f, 9.0f, 40.0f, 150.0f};

        vector<vec4> cases;
        for (const glm::mat4& projection : projections)
        {
            for (const f32 depth : depths)
            {
                const vec4 clip = projection * vec4(0.0f, 0.0f, -depth, 1.0f);
                cases.emplace_back(projection[2][2], projection[3][2], projection[2][3],
                                   projection[3][3]);
                cases.emplace_back(clip.z / clip.w, 0.0f, 0.0f, 0.0f);
            }
        }
        const vector<vec4> results = harness.Run(Mode::ViewDepth, cases, 10, 1);
        f32 worst = 0.0f;
        for (usize p = 0; p < projections.size(); ++p)
        {
            for (usize d = 0; d < depths.size(); ++d)
            {
                const f32 got = results[p * depths.size() + d].x;
                worst = std::max(worst, std::abs(got - depths[d]) / depths[d]);
            }
        }
        CHECK(worst < 1e-4f);
    }
}
