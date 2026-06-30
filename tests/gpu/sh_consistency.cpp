// CPU↔GPU spherical-harmonics consistency: the gate for the dynamic SH skylight. The
// deferred lighting pass reconstructs irradiance on the GPU through Veng/sh.slang's
// ShEvalIrradiance, while the coefficients are projected and convolved on the CPU through
// SphericalHarmonics.h's EvalIrradiance. A basis/normalization drift between the two is the
// single biggest SH footgun and is invisible to eyeballing, so this dispatches the actual
// core GPU eval (the sh_eval.comp test shader includes the same Veng/sh.slang the engine
// does) over the exact golden coefficient set and normals the CPU spherical_harmonics unit
// test pins, and asserts the GPU result matches both the CPU EvalIrradiance and the
// hand-computed golden constants.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <array>
#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Math/SphericalHarmonics.h>
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
    // The fixed golden coefficient set the CPU spherical_harmonics unit test pins, replicated
    // across RGB so the GPU result's three channels each carry the single-channel golden.
    constexpr std::array<f32, 9> GoldenCoefficients{0.5f,  0.1f,  -0.2f,  0.3f, 0.05f,
                                                    -0.1f, 0.15f, -0.07f, 0.02f};

    // The same normals the CPU golden test evaluates at; the .x of each GPU result must match
    // the CPU EvalIrradiance and the hand-computed golden below.
    const std::array<vec3, 4> GoldenNormals{vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f),
                                            vec3(0.0f, 0.0f, 1.0f),
                                            glm::normalize(vec3(1.0f, 1.0f, 1.0f))};

    // The checked-in golden EvalIrradiance values (single channel) at GoldenNormals.
    constexpr std::array<f32, 4> GoldenIrradiance{0.25124508f, 0.13167352f, 0.13794450f,
                                                  0.15376459f};

    // The test shader pack's sh_eval.comp id.
    constexpr AssetId ShEvalCompId{8011};

    struct ShEvalPush
    {
        u32 NormalCount;
    };
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "GPU ShEvalIrradiance matches the CPU eval and the checked-in goldens")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mounted = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mounted, mounted.error());

    const auto shaderAsset = assets.LoadSync<Shader>(ShEvalCompId);
    REQUIRE_MESSAGE(shaderAsset, shaderAsset.error().Detail);

    const u32 normalCount = static_cast<u32>(GoldenNormals.size());

    // Pack the coefficients and normals as vec4 (the std140/byte-addressed layout the shader
    // reads), build the CPU reference set from the same coefficients.
    Sh9 cpuSh{};
    std::array<vec4, 9> coefficientData{};
    for (u32 i = 0; i < ShCoefficientCount; ++i)
    {
        cpuSh.Coefficients[i] = vec3(GoldenCoefficients[i]);
        coefficientData[i] = vec4(vec3(GoldenCoefficients[i]), 0.0f);
    }
    std::array<vec4, 4> normalData{};
    for (u32 i = 0; i < normalCount; ++i)
    {
        normalData[i] = vec4(GoldenNormals[i], 0.0f);
    }

    auto coefficientBuffer =
        Buffer::Create(Context, {
                                    .Name = "SH Coefficients",
                                    .Size = sizeof(coefficientData),
                                    .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                });
    coefficientBuffer->UploadSync(
        std::span(reinterpret_cast<const u8*>(coefficientData.data()), sizeof(coefficientData)));

    auto normalBuffer =
        Buffer::Create(Context, {
                                    .Name = "SH Normals",
                                    .Size = sizeof(normalData),
                                    .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                });
    normalBuffer->UploadSync(
        std::span(reinterpret_cast<const u8*>(normalData.data()), sizeof(normalData)));

    auto resultBuffer =
        Buffer::Create(Context, {
                                    .Name = "SH Results",
                                    .Size = static_cast<u64>(normalCount) * sizeof(vec4),
                                    .Usage = BufferUsage::Storage | BufferUsage::TransferSrc,
                                });

    auto setLayout =
        DescriptorSetLayout::Create(Context, {
                                                 .Name = "SH Eval Set Layout",
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
                                                         {.Binding = 2,
                                                          .Type = DescriptorType::StorageBuffer,
                                                          .Count = 1,
                                                          .Stages = ShaderStage::Compute},
                                                     },
                                             });

    auto pipelineLayout =
        PipelineLayout::Create(Context, {
                                            .Name = "SH Eval Layout",
                                            .DescriptorSetLayouts = {setLayout},
                                            .PushConstantRanges = {{.Stages = ShaderStage::Compute,
                                                                    .Offset = 0,
                                                                    .Size = sizeof(ShEvalPush)}},
                                        });

    auto pipeline = ComputePipeline::Create(
        Context,
        {
            .Name = "SH Eval Pipeline",
            .PipelineLayout = pipelineLayout,
            .ShaderStage = {.Stage = ShaderStage::Compute, .Module = shaderAsset->Get()->Module},
        });

    auto set = DescriptorSet::Create(Context, {
                                                  .Name = "SH Eval Set",
                                                  .Layout = setLayout,
                                              });
    set->Write(0, coefficientBuffer);
    set->Write(1, normalBuffer);
    set->Write(2, resultBuffer);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            cmd.BindPipeline(pipeline);
            // Set 0 is reserved for the bindless registry; the hand-built set binds at 1.
            cmd.BindDescriptorSets({
                .Sets = {set},
                .FirstSet = 1,
                .PipelineBindPoint = PipelineBindPoint::Compute,
            });
            cmd.PushConstants(ShEvalPush{.NormalCount = normalCount});
            cmd.Dispatch(normalCount, 1, 1);
        });

    const vector<u8> downloaded = resultBuffer->Download();
    REQUIRE(downloaded.size() == static_cast<usize>(normalCount) * sizeof(vec4));

    std::array<vec4, 4> gpuResults{};
    std::memcpy(gpuResults.data(), downloaded.data(), downloaded.size());

    for (u32 i = 0; i < normalCount; ++i)
    {
        const vec3 cpu = EvalIrradiance(cpuSh, GoldenNormals[i]);
        const vec3 gpu = vec3(gpuResults[i]);

        // GPU vs CPU eval: the consistency gate proper.
        CHECK(std::abs(gpu.x - cpu.x) <= 1e-4f);
        CHECK(std::abs(gpu.y - cpu.y) <= 1e-4f);
        CHECK(std::abs(gpu.z - cpu.z) <= 1e-4f);

        // GPU vs the hand-computed golden: pins the GPU to the same checked-in numbers the CPU
        // test does, so a basis change that moved both in lockstep would still be caught.
        CHECK(std::abs(gpu.x - GoldenIrradiance[i]) <= 1e-4f);
    }
}
