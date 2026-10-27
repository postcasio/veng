// One Slang source, two targets: tests/shaders/host/gradient_noise.slang's fractal sum is
// dispatched on the GPU (gradient_noise.comp.slang includes it) and evaluated on the host (the
// same file compiled by veng_add_host_shader_module), and the two must agree to rounding. The
// points are stratified to reach the boundary cases a lattice function is likeliest to split on:
// exact lattice points, negative cells, and one ulp either side of a lattice plane.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include <doctest/doctest.h>

#include <GradientNoise.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The test shader pack's gradient_noise.comp id.
    constexpr AssetId GradientNoiseCompId{0x97B525975A19E22AULL};

    // The bounds a consumer may rely on, with a margin over the worst measured across 6,016
    // points (value 9e-7, gradient 5.3e-6 at 16 octaves, MoltenVK with fast math).
    constexpr f32 ValueBound = 1e-5f;
    constexpr f32 GradientBound = 1e-4f;

    struct GradientNoisePush
    {
        u32 PointCount;
    };

    struct NoisePoint
    {
        vec3 Position;
        u32 Octaves;
    };

    // 256 points: 64 lattice points, 96 points one ulp off a lattice plane, and 96 spread over
    // |p| <= 256, each set cycling the octave counts 1, 6 and 16.
    vector<NoisePoint> AgreementPoints()
    {
        constexpr std::array<u32, 3> OctaveCounts{1u, 6u, 16u};
        vector<NoisePoint> points;
        const auto octaves = [&] { return OctaveCounts[points.size() % OctaveCounts.size()]; };
        for (int i = 0; i < 64; ++i)
        {
            const vec3 lattice(static_cast<f32>((i * 37) % 512 - 256),
                               static_cast<f32>((i * 91) % 512 - 256),
                               static_cast<f32>((i * 53) % 128 - 64));
            points.push_back({.Position = lattice, .Octaves = octaves()});
        }
        for (int i = 0; i < 48; ++i)
        {
            const f32 plane = static_cast<f32>((i * 29) % 400 - 200);
            const f32 a = static_cast<f32>(i) * 3.7f - 90.0f;
            const f32 b = 50.0f - static_cast<f32>(i) * 2.3f;
            const f32 below = std::nextafter(plane, -std::numeric_limits<f32>::infinity());
            const f32 above = std::nextafter(plane, std::numeric_limits<f32>::infinity());
            points.push_back({.Position = vec3(below, a, b), .Octaves = octaves()});
            points.push_back({.Position = vec3(a, above, b), .Octaves = octaves()});
        }
        for (int i = 0; i < 96; ++i)
        {
            const f32 t = static_cast<f32>(i);
            const vec3 spread(std::fmod(t * 61.803f, 512.0f) - 256.0f,
                              std::fmod(t * 41.421f + 17.0f, 512.0f) - 256.0f,
                              std::fmod(t * 73.205f + 101.0f, 512.0f) - 256.0f);
            points.push_back({.Position = spread, .Octaves = octaves()});
        }
        return points;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "A host-compiled Slang module agrees with the same source on the GPU")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mounted = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mounted, mounted.error());
    const auto shaderAsset = assets.LoadSync<Shader>(GradientNoiseCompId);
    REQUIRE_MESSAGE(shaderAsset, shaderAsset.error().Detail);

    const vector<NoisePoint> points = AgreementPoints();
    const u32 count = static_cast<u32>(points.size());

    // xyz the point, w its octave count's bits: the float4 layout the shader loads.
    vector<vec4> packed(count);
    for (u32 i = 0; i < count; ++i)
    {
        f32 octaveBits = 0.0f;
        std::memcpy(&octaveBits, &points[i].Octaves, sizeof(octaveBits));
        packed[i] = vec4(points[i].Position, octaveBits);
    }
    const u64 bytes = static_cast<u64>(count) * sizeof(vec4);

    const auto pointBuffer =
        Buffer::Create(Context, {
                                    .Name = "Noise Points",
                                    .Size = bytes,
                                    .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                });
    pointBuffer->UploadSync(std::span(reinterpret_cast<const u8*>(packed.data()), bytes));
    const auto resultBuffer =
        Buffer::Create(Context, {
                                    .Name = "Noise Results",
                                    .Size = bytes,
                                    .Usage = BufferUsage::Storage | BufferUsage::TransferSrc,
                                });

    auto setLayout =
        DescriptorSetLayout::Create(Context, {
                                                 .Name = "Noise Set Layout",
                                                 .Bindings =
                                                     {
                                                         {.Binding = 0,
                                                          .Type = DescriptorType::StorageBuffer,
                                                          .Count = 1,
                                                          .Stages = ShaderStage::Compute},
                                                         {.Binding = 1,
                                                          .Type = DescriptorType::StorageBuffer,
                                                          .Count = 1,
                                                          .Stages = ShaderStage::Compute},
                                                     },
                                             });
    const auto pipelineLayout = PipelineLayout::Create(
        Context, {
                     .Name = "Noise Layout",
                     .DescriptorSetLayouts = {setLayout},
                     .PushConstantRanges = {{.Stages = ShaderStage::Compute,
                                             .Offset = 0,
                                             .Size = sizeof(GradientNoisePush)}},
                 });
    auto pipeline = ComputePipeline::Create(
        Context,
        {
            .Name = "Noise Pipeline",
            .PipelineLayout = pipelineLayout,
            .ShaderStage = {.Stage = ShaderStage::Compute, .Module = shaderAsset->Get()->Module},
        });
    auto set = DescriptorSet::Create(Context, {
                                                  .Name = "Noise Set",
                                                  .Layout = setLayout,
                                              });
    set->Write(0, pointBuffer);
    set->Write(1, resultBuffer);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            cmd.BindPipeline(pipeline);
            // Sets 0-2 are the bindless registries; the hand-built set binds at 3.
            cmd.BindDescriptorSets({
                .Sets = {set},
                .FirstSet = 3,
                .PipelineBindPoint = PipelineBindPoint::Compute,
            });
            cmd.PushConstants(GradientNoisePush{.PointCount = count});
            cmd.Dispatch((count + 63) / 64, 1, 1);
        });

    const vector<u8> downloaded = resultBuffer->Download();
    REQUIRE(downloaded.size() == bytes);
    vector<vec4> gpu(count);
    std::memcpy(gpu.data(), downloaded.data(), bytes);

    f32 worstValue = 0.0f;
    f32 worstGradient = 0.0f;
    for (u32 i = 0; i < count; ++i)
    {
        const vec4 host = GradientNoise::FbmF32(points[i].Position, points[i].Octaves);
        const vec4 error = glm::abs(host - gpu[i]);
        worstValue = std::max(worstValue, error.x);
        worstGradient = std::max({worstGradient, error.y, error.z, error.w});
    }
    INFO("worst value error " << worstValue << ", worst gradient error " << worstGradient);
    CHECK(worstValue <= ValueBound);
    CHECK(worstGradient <= GradientBound);
}
