// The Sphere-light LTC integrator, evaluated by the shader that ships it. A uniform sphere of
// radius r at distance d irradiates a facing Lambertian surface with a clamped-cosine form factor of
// exactly sin^2(theta), sin(theta) = r/d — the analytic sphere irradiance, independent of angular
// size. Veng/ltc.slang integrates the sphere as its silhouette (tangent) disk so the form factor
// matches that analytic value at every size; a disk built at the sphere centre with the sphere's own
// radius subtends only atan(r/d) and under-integrates by up to 2x as the source fills the sky. That
// magnitude is invisible in any output short of a lit scene, so this dispatches the same include the
// deferred lighting pass uses and asserts the form factor directly.
//
// The claim is a property, not a portrait: the diffuse form factor equals sin^2(theta) across the
// whole angular range (to within the 16-gon discretization), which is exactly the correctness the
// silhouette-disk construction buys — and which the centre-disk construction fails at large sizes.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <array>
#include <cstring>

#include <doctest/doctest.h>

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
    // The test shader pack's ltc_sphere.comp id.
    constexpr AssetId LtcSphereCompId{8016};

    struct LtcSpherePush
    {
        u32 CaseCount;
    };

    // Source angular size as r/d (emitter radius over distance to the shading point). The shading
    // point faces the sphere head-on, so the diffuse form factor is at its analytic maximum.
    const std::array<vec4, 6> Cases{
        vec4(0.05f, 0.0f, 0.0f, 0.0f), // Tiny: the small-angle limit.
        vec4(0.20f, 0.0f, 0.0f, 0.0f),
        vec4(0.50f, 0.0f, 0.0f, 0.0f),
        vec4(0.70f, 0.0f, 0.0f, 0.0f),
        vec4(0.866f, 0.0f, 0.0f, 0.0f), // The star at ~1.16 stellar radii: r/d = 0.866, sin^2 = 0.75.
        vec4(0.95f, 0.0f, 0.0f, 0.0f), // A source nearly filling the facing hemisphere.
    };
    constexpr usize Tiny = 0;
    constexpr usize Star = 4;
    constexpr usize Widest = 5;

    // Result lanes: (diffSum, sin^2(theta), 0, 0).
    constexpr int DiffSum = 0;
    constexpr int Sin2 = 1;
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "The Sphere-light LTC diffuse form factor matches the analytic sphere irradiance")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mounted = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mounted, mounted.error());

    const auto shaderAsset = assets.LoadSync<Shader>(LtcSphereCompId);
    REQUIRE_MESSAGE(shaderAsset, shaderAsset.error().Detail);

    const u32 caseCount = static_cast<u32>(Cases.size());

    auto caseBuffer =
        Buffer::Create(Context, {
                                    .Name = "LTC Sphere Cases",
                                    .Size = sizeof(Cases),
                                    .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                });
    caseBuffer->UploadSync(std::span(reinterpret_cast<const u8*>(Cases.data()), sizeof(Cases)));

    auto resultBuffer =
        Buffer::Create(Context, {
                                    .Name = "LTC Sphere Results",
                                    .Size = static_cast<u64>(caseCount) * sizeof(vec4),
                                    .Usage = BufferUsage::Storage | BufferUsage::TransferSrc,
                                });

    auto setLayout =
        DescriptorSetLayout::Create(Context, {
                                                 .Name = "LTC Sphere Set Layout",
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

    auto pipelineLayout =
        PipelineLayout::Create(Context, {
                                            .Name = "LTC Sphere Layout",
                                            .DescriptorSetLayouts = {setLayout},
                                            .PushConstantRanges = {{.Stages = ShaderStage::Compute,
                                                                    .Offset = 0,
                                                                    .Size = sizeof(LtcSpherePush)}},
                                        });

    auto pipeline = ComputePipeline::Create(
        Context,
        {
            .Name = "LTC Sphere Pipeline",
            .PipelineLayout = pipelineLayout,
            .ShaderStage = {.Stage = ShaderStage::Compute, .Module = shaderAsset->Get()->Module},
        });

    auto set = DescriptorSet::Create(Context, {
                                                  .Name = "LTC Sphere Set",
                                                  .Layout = setLayout,
                                              });
    set->Write(0, caseBuffer);
    set->Write(1, resultBuffer);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            cmd.BindPipeline(pipeline);
            cmd.BindDescriptorSets({
                .Sets = {set},
                .FirstSet = 3,
                .PipelineBindPoint = PipelineBindPoint::Compute,
            });
            cmd.PushConstants(LtcSpherePush{.CaseCount = caseCount});
            cmd.Dispatch(caseCount, 1, 1);
        });

    const vector<u8> downloaded = resultBuffer->Download();
    REQUIRE(downloaded.size() == static_cast<usize>(caseCount) * sizeof(vec4));

    std::array<vec4, 6> results{};
    std::memcpy(results.data(), downloaded.data(), downloaded.size());

    // The form factor equals the analytic sphere irradiance sin^2(theta) at every angular size, to
    // within the 16-gon disk's slight under-count. The tolerance admits that (~2.6% at the small
    // end) while excluding the centre-disk construction, which returns sin^2(atan(r/d)) — 0.43 at
    // the star's r/d = 0.866 against the correct 0.75, a 43% deficit no honest tolerance passes.
    for (usize i = 0; i < results.size(); ++i)
    {
        CHECK(results[i][DiffSum] == doctest::Approx(results[i][Sin2]).epsilon(0.05f));
    }

    // The form factor rises monotonically with the source's angular size.
    for (usize i = 1; i < results.size(); ++i)
    {
        CHECK(results[i][DiffSum] > results[i - 1][DiffSum]);
    }

    // The discriminating floor, stated in the failure it guards: the centre-disk construction cannot
    // reach the star's true form factor — its disk subtends only atan(0.866) = 40.9 degrees, so it
    // tops out near 0.43. The silhouette disk reaches 0.75.
    CHECK(results[Star][DiffSum] > 0.6f);

    // The small-angle limit is where the two constructions agree, so it pins the shared normalization
    // rather than the fix: a tiny facing source of angular size r/d returns very nearly (r/d)^2.
    CHECK(results[Tiny][DiffSum] == doctest::Approx(0.0025f).epsilon(0.05f));

    // A source nearly filling the facing hemisphere approaches, but does not exceed, unit coverage.
    CHECK(results[Widest][DiffSum] < 1.0f);
}
