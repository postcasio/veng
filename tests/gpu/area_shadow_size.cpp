// The area-light PCSS source-size policy, evaluated by the shader that ships it. The deferred
// lighting pass sizes an area light's blocker search and penumbra filter from how large the source
// subtends at the receiver, and that estimator is a single-averaged-blocker approximation with a
// bounded domain: fed a source comparable to its own distance it searches a disc far wider than
// the filter can ever cover, averages occluders the filter never samples, and reports a receiver
// standing in full light as fully occluded. Veng/area_shadow.slang owns the two decisions that
// keep it inside its domain — the ratio is against the distance to the light, and it is capped —
// and neither is visible in any output short of a lit scene, so this dispatches the same include
// the lighting pass uses and asserts the policy directly.
//
// The claims are written so no constant of the policy is restated here: the cap is read off the
// shader's own answer for an absurd source, and every check is a relation between answers.
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
    // The test shader pack's area_shadow_size.comp id.
    constexpr AssetId AreaShadowSizeCompId{8015};

    struct AreaShadowSizePush
    {
        u32 CaseCount;
    };

    // (source radius, receiver→light distance, penumbra ratio). The named indices below read the
    // results out.
    const std::array<vec4, 7> Cases{
        vec4(1.0f, 1000.0f, 0.5f, 0.0f),     // Tiny: far inside the cap.
        vec4(2.0f, 2000.0f, 0.5f, 0.0f),     // TinyScaled: the same ratio at twice the size.
        vec4(1.0f, 500.0f, 0.5f, 0.0f),      // Double: twice the ratio, still inside the cap.
        vec4(100.0f, 10.0f, 1.0f, 0.0f),     // Huge: ten times its own distance.
        vec4(10000.0f, 10.0f, 1.0f, 0.0f),   // Absurd: a thousand times its own distance.
        vec4(10000.0f, 10.0f, 100.0f, 0.0f), // Widest: an absurd source at an absurd gap.
        vec4(0.0f, 10.0f, 0.5f, 0.0f),       // Unsized: the punctual case, which carries no size.
    };
    constexpr usize Tiny = 0;
    constexpr usize TinyScaled = 1;
    constexpr usize Double = 2;
    constexpr usize Huge = 3;
    constexpr usize Absurd = 4;
    constexpr usize Widest = 5;
    constexpr usize Unsized = 6;

    // Result lanes.
    constexpr int SizeFactor = 0;
    constexpr int SearchTexels = 1;
    constexpr int FilterTexels = 2;
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "The area-shadow source size is an angular ratio, capped where PCSS degrades")
{
    AssetManager assets(Context, Tasks, Types);
    const VoidResult mounted = assets.Mount(path(TEST_SHADER_PACK));
    REQUIRE_MESSAGE(mounted, mounted.error());

    const auto shaderAsset = assets.LoadSync<Shader>(AreaShadowSizeCompId);
    REQUIRE_MESSAGE(shaderAsset, shaderAsset.error().Detail);

    const u32 caseCount = static_cast<u32>(Cases.size());

    auto caseBuffer =
        Buffer::Create(Context, {
                                    .Name = "Area Shadow Size Cases",
                                    .Size = sizeof(Cases),
                                    .Usage = BufferUsage::Storage | BufferUsage::TransferDst,
                                });
    caseBuffer->UploadSync(std::span(reinterpret_cast<const u8*>(Cases.data()), sizeof(Cases)));

    auto resultBuffer =
        Buffer::Create(Context, {
                                    .Name = "Area Shadow Size Results",
                                    .Size = static_cast<u64>(caseCount) * sizeof(vec4),
                                    .Usage = BufferUsage::Storage | BufferUsage::TransferSrc,
                                });

    auto setLayout =
        DescriptorSetLayout::Create(Context, {
                                                 .Name = "Area Shadow Size Set Layout",
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

    auto pipelineLayout = PipelineLayout::Create(
        Context, {
                     .Name = "Area Shadow Size Layout",
                     .DescriptorSetLayouts = {setLayout},
                     .PushConstantRanges = {{.Stages = ShaderStage::Compute,
                                             .Offset = 0,
                                             .Size = sizeof(AreaShadowSizePush)}},
                 });

    auto pipeline = ComputePipeline::Create(
        Context,
        {
            .Name = "Area Shadow Size Pipeline",
            .PipelineLayout = pipelineLayout,
            .ShaderStage = {.Stage = ShaderStage::Compute, .Module = shaderAsset->Get()->Module},
        });

    auto set = DescriptorSet::Create(Context, {
                                                  .Name = "Area Shadow Size Set",
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
            cmd.PushConstants(AreaShadowSizePush{.CaseCount = caseCount});
            cmd.Dispatch(caseCount, 1, 1);
        });

    const vector<u8> downloaded = resultBuffer->Download();
    REQUIRE(downloaded.size() == static_cast<usize>(caseCount) * sizeof(vec4));

    std::array<vec4, 7> results{};
    std::memcpy(results.data(), downloaded.data(), downloaded.size());

    // The ratio is the source radius over the distance to the light, and nothing else. A shadow
    // view's fitted far plane is not a distance to the light, so a size computed against one is
    // not an angular size at all — the two cases below fix the ratio while the distance moves.
    CHECK(results[Tiny][SizeFactor] == doctest::Approx(1.0f / 1000.0f).epsilon(1e-4f));
    CHECK(results[TinyScaled][SizeFactor] == doctest::Approx(results[Tiny][SizeFactor]));
    CHECK(results[Double][SizeFactor] == doctest::Approx(2.0f * results[Tiny][SizeFactor]));

    // The cap: the shader's own answer for a source ten times its distance, which nothing here
    // restates. It is a real cap rather than a saturation to unit ratio, and it still admits a
    // size, so a source past it keeps casting a shadow instead of losing one.
    const f32 cap = results[Huge][SizeFactor];
    CHECK(cap > 0.0f);
    CHECK(cap < 1.0f);
    CHECK(cap > results[Double][SizeFactor]);

    // Past the cap the answer clamps rather than growing: a source a hundred times larger again
    // lands on the same size.
    CHECK(results[Absurd][SizeFactor] == doctest::Approx(cap));

    // The degeneracy the cap removes, stated in the widths the size drives: the blocker search of
    // the largest source admitted stays inside the widest disc the penumbra filter can cover, so
    // every occluder the average is built from is one the filter can actually sample.
    const f32 widestFilter = results[Widest][FilterTexels];
    CHECK(results[Absurd][SearchTexels] <= widestFilter);

    // And the filter is not itself doing the capping: a capped source at a full receiver→blocker
    // gap still asks for less than the filter's clamp, so the shadow it casts is a penumbra the
    // estimator chose rather than a width the clamp imposed.
    CHECK(results[Absurd][FilterTexels] < widestFilter);

    // An unsized source — every punctual light, and any area light that carries no shadow radius —
    // is unchanged by all of this: no size, the narrowest search, the narrowest filter.
    CHECK(results[Unsized][SizeFactor] == 0.0f);
    CHECK(results[Unsized][SearchTexels] < results[Tiny][SearchTexels]);
    CHECK(results[Unsized][FilterTexels] <= results[Tiny][FilterTexels]);
}
