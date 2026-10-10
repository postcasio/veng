// The bloom bright pass — level 0 of the down sweep — driven directly on exact texel content, for
// both kernels. Three properties of what it seeds into mip 0, each summed over the whole level:
//
//   - below the firefly ceiling it conserves a source's light, so a small bright source seeds the
//     same energy at every sub-texel offset (both kernels give every source texel the same total
//     weight, a quarter for a 2:1 downsample, wherever it falls on the destination grid);
//   - one extreme texel seeds no more than the ceiling's worth, and keeps its hue;
//   - a NaN or an inf seeds nothing, and leaves the rest of the level finite.

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

#include <doctest/doctest.h>
#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The core pack's two bloom downsample kernels.
    constexpr AssetId BloomDownCompId{0x5B8811BEAC5D9C3BULL};
    constexpr AssetId BloomDownKawaseCompId{0xCB1AA796A1E3BBEFULL};

    // Matches bloom_bright_pass.slang's push block.
    struct BloomDownPush
    {
        uvec2 DestExtent;
        vec2 SourceScaleUV;
        vec2 SourceMaxUV;
        f32 BrightPass;
        f32 Threshold;
        u32 MaskTexture;
        u32 MaskSampler;
        u32 MaskEnabled;
        f32 FireflyCeiling;
    };

    constexpr uvec2 SourceExtent{64, 64};
    constexpr uvec2 DestExtent{32, 32};

    // A knee low enough that the bright-pass gain is saturated across every texel a test source
    // reaches, so what is measured is the tap conditioning and the kernel alone.
    constexpr f32 OpenThreshold = 1.0e-4f;

    f32 Luma(const vec3 c)
    {
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    }

    // The sum over mip 0 of each channel, and how many of its texels are not finite.
    struct LevelSum
    {
        dvec3 Rgb{0.0};
        u32 NonFinite = 0;

        [[nodiscard]] f64 Luma() const { return 0.2126 * Rgb.r + 0.7152 * Rgb.g + 0.0722 * Rgb.b; }
    };

    // Runs one kernel's level 0 over a source field and sums the level it writes.
    class BrightPassRig
    {
    public:
        explicit BrightPassRig(Renderer::Context& context, AssetManager& assets)
            : m_Context(context)
        {
            m_SetLayout = DescriptorSetLayout::Create(
                m_Context, {.Name = "Bloom Bright Pass Set Layout",
                            .Bindings = {{.Binding = 0,
                                          .Type = DescriptorType::SampledImage,
                                          .Count = 1,
                                          .Stages = ShaderStage::Compute},
                                         {.Binding = 1,
                                          .Type = DescriptorType::Sampler,
                                          .Count = 1,
                                          .Stages = ShaderStage::Compute},
                                         {.Binding = 2,
                                          .Type = DescriptorType::StorageImage,
                                          .Count = 1,
                                          .Stages = ShaderStage::Compute}}});
            m_Layout = PipelineLayout::Create(
                m_Context, {.Name = "Bloom Bright Pass Layout",
                            .DescriptorSetLayouts = {m_SetLayout},
                            .PushConstantRanges = {
                                PushConstantRange::Of<BloomDownPush>(ShaderStage::Compute)}});
            const std::array<AssetId, 2> ids{BloomDownCompId, BloomDownKawaseCompId};
            for (usize kernel = 0; kernel < ids.size(); ++kernel)
            {
                const AssetResult<AssetHandle<Shader>> shader =
                    assets.LoadSync<Shader>(ids[kernel]);
                REQUIRE(shader.has_value());
                m_Pipelines[kernel] = ComputePipeline::Create(
                    m_Context, {.Name = "Bloom Bright Pass Pipeline",
                                .PipelineLayout = m_Layout,
                                .ShaderStage = {.Stage = ShaderStage::Compute,
                                                .Module = shader->Get()->Module}});
            }
            m_Sampler = Sampler::Create(m_Context, {
                                                       .Name = "Bloom Bright Pass Sampler",
                                                       .MagFilter = Filter::Linear,
                                                       .MinFilter = Filter::Linear,
                                                       .MipmapMode = MipmapMode::Nearest,
                                                       .AddressModeU = AddressMode::ClampToEdge,
                                                       .AddressModeV = AddressMode::ClampToEdge,
                                                       .AddressModeW = AddressMode::ClampToEdge,
                                                       .AnisotropyEnabled = false,
                                                   });
        }

        // @p kernel 0 is the COD 13-tap, 1 the Dual Kawase 5-tap.
        [[nodiscard]] LevelSum Run(const usize kernel, const std::vector<vec3>& field,
                                   const f32 ceiling)
        {
            REQUIRE(field.size() == static_cast<usize>(SourceExtent.x) * SourceExtent.y);
            std::vector<u16> halves(field.size() * 4);
            for (usize i = 0; i < field.size(); ++i)
            {
                for (u32 c = 0; c < 3; ++c)
                {
                    halves[i * 4 + c] = ToHalf(field[i][static_cast<i32>(c)]);
                }
                halves[i * 4 + 3] = glm::packHalf1x16(1.0f);
            }

            const Ref<Image> source =
                Image::Create(m_Context, {
                                             .Name = "Bloom Bright Pass Source",
                                             .Extent = {SourceExtent.x, SourceExtent.y, 1},
                                             .Format = Format::RGBA16Sfloat,
                                             .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                                         });
            source->UploadSync(
                std::span(reinterpret_cast<const u8*>(halves.data()), halves.size() * sizeof(u16)));
            const Ref<ImageView> sourceView = ImageView::Create(
                m_Context, {.Name = "Bloom Bright Pass Source View", .Image = source});
            const Ref<Image> dest =
                Image::Create(m_Context, {
                                             .Name = "Bloom Bright Pass Mip 0",
                                             .Extent = {DestExtent.x, DestExtent.y, 1},
                                             .Format = Format::RGBA16Sfloat,
                                             .Usage = ImageUsage::Storage | ImageUsage::TransferSrc,
                                         });
            const Ref<ImageView> destView = ImageView::Create(
                m_Context, {.Name = "Bloom Bright Pass Mip 0 View", .Image = dest});
            const Ref<DescriptorSet> set = DescriptorSet::Create(
                m_Context, {.Name = "Bloom Bright Pass Set", .Layout = m_SetLayout});
            set->Write(0, sourceView);
            set->Write(1, m_Sampler);
            set->Write(2, destView);

            const Ref<ComputePipeline> pipeline = m_Pipelines[kernel];
            const BloomDownPush push{
                .DestExtent = DestExtent,
                .SourceScaleUV = vec2(1.0f),
                .SourceMaxUV = (vec2(SourceExtent) - 0.5f) / vec2(SourceExtent),
                .BrightPass = 1.0f,
                .Threshold = OpenThreshold,
                .MaskTexture = 0,
                .MaskSampler = 0,
                .MaskEnabled = 0,
                .FireflyCeiling = ceiling,
            };
            m_Context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    RenderGraph graph(m_Context);
                    const ResourceId sourceId = graph.Import("Bloom Bright Pass Source");
                    const ResourceId destId = graph.Import("Bloom Bright Pass Mip 0");
                    graph.AddComputePass("Bloom Bright Pass")
                        .Sample(sourceId)
                        .StorageWrite(destId)
                        .Execute(
                            [&](PassContext& ctx)
                            {
                                CommandBuffer& c = ctx.Cmd();
                                c.BindPipeline(pipeline);
                                // The kernel declares the registry's arrays for its mask read, so
                                // set 0 is part of its layout even with the mask off.
                                m_Context.GetBindlessRegistry().Bind(c, PipelineBindPoint::Compute);
                                c.BindDescriptorSets(DescriptorSetBindInfo{
                                    .Sets = {set},
                                    .FirstSet = 3,
                                    .PipelineBindPoint = PipelineBindPoint::Compute,
                                });
                                c.PushConstants(push);
                                c.Dispatch((DestExtent.x + 7) / 8, (DestExtent.y + 7) / 8, 1);
                            });
                    graph.Compile()->Execute(cmd, {{.Id = sourceId, .View = sourceView},
                                                   {.Id = destId, .View = destView}});
                });

            const vector<u8> bytes = dest->Download();
            REQUIRE(bytes.size() == static_cast<usize>(DestExtent.x) * DestExtent.y * 8);
            std::vector<u16> texels(bytes.size() / sizeof(u16));
            std::memcpy(texels.data(), bytes.data(), bytes.size());
            LevelSum sum;
            for (usize i = 0; i < texels.size(); i += 4)
            {
                const vec3 c{glm::unpackHalf1x16(texels[i]), glm::unpackHalf1x16(texels[i + 1]),
                             glm::unpackHalf1x16(texels[i + 2])};
                if (!std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b))
                {
                    ++sum.NonFinite;
                    continue;
                }
                sum.Rgb += dvec3(c);
            }
            return sum;
        }

    private:
        // Half-float bits for any value, the non-finite ones spelled out rather than trusted to a
        // conversion routine.
        static u16 ToHalf(const f32 value)
        {
            if (std::isnan(value))
            {
                return 0x7E00u;
            }
            if (std::isinf(value))
            {
                return value > 0.0f ? 0x7C00u : 0xFC00u;
            }
            return static_cast<u16>(glm::packHalf1x16(value));
        }

        Renderer::Context& m_Context;
        Ref<DescriptorSetLayout> m_SetLayout;
        Ref<PipelineLayout> m_Layout;
        std::array<Ref<ComputePipeline>, 2> m_Pipelines;
        Ref<Sampler> m_Sampler;
    };

    std::vector<vec3> EmptyField()
    {
        return std::vector<vec3>(static_cast<usize>(SourceExtent.x) * SourceExtent.y, vec3(0.0f));
    }

    vec3& At(std::vector<vec3>& field, const u32 x, const u32 y)
    {
        return field[static_cast<usize>(y) * SourceExtent.x + x];
    }

    // A grey box @p width texels wide with its corner at @p corner (in texels), each texel taking
    // @p peak times the fraction of it the box covers — a small source rendered as it would be at
    // that sub-texel position.
    std::vector<vec3> BoxSource(const vec2 corner, const f32 width, const f32 peak)
    {
        std::vector<vec3> field = EmptyField();
        for (u32 y = 0; y < SourceExtent.y; ++y)
        {
            for (u32 x = 0; x < SourceExtent.x; ++x)
            {
                const vec2 lo = glm::max(vec2(x, y), corner);
                const vec2 hi = glm::min(vec2(x + 1, y + 1), corner + width);
                const vec2 overlap = glm::max(hi - lo, vec2(0.0f));
                At(field, x, y) = vec3(peak * overlap.x * overlap.y);
            }
        }
        return field;
    }

    f64 FieldLuma(const std::vector<vec3>& field)
    {
        f64 sum = 0.0;
        for (const vec3& c : field)
        {
            sum += Luma(c);
        }
        return sum;
    }
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "bloom bright pass: a small source under the ceiling seeds the same light at "
                  "every sub-texel offset")
{
    AssetManager assets(Context, Tasks, Types);
    BrightPassRig rig(Context, assets);

    // 24 is a bright small source (several times past the tone curves' white) whose every tap stays
    // under half the default ceiling, so the average is linear for it.
    constexpr f32 Peak = 24.0f;
    constexpr f32 Ceiling = 64.0f;

    for (usize kernel = 0; kernel < 2; ++kernel)
    {
        CAPTURE(kernel);
        // The worst departure of the seeded light from a quarter of the source's, over one- to
        // three-texel sources stepped across two texels in quarters on both axes.
        f64 worst = 0.0;
        for (const f32 width : {1.0f, 2.0f, 3.0f})
        {
            for (u32 step = 0; step < 8; ++step)
            {
                const f32 offset = 0.25f * static_cast<f32>(step);
                const std::vector<vec3> field =
                    BoxSource(vec2(30.0f + offset, 29.0f + 0.625f * offset), width, Peak);
                const LevelSum level = rig.Run(kernel, field, Ceiling);
                REQUIRE(level.NonFinite == 0u);
                const f64 expected = FieldLuma(field) * 0.25;
                worst = std::max(worst, std::abs(level.Luma() - expected) / expected);
            }
        }
        CHECK(worst < 0.01);
    }
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "bloom bright pass: one extreme texel seeds no more than the ceiling, in its "
                  "own hue")
{
    AssetManager assets(Context, Tasks, Types);
    BrightPassRig rig(Context, assets);

    // A pure-red firefly near the half-float maximum: what a degenerate shading path writes.
    constexpr f32 Firefly = 6.0e4f;

    for (usize kernel = 0; kernel < 2; ++kernel)
    {
        CAPTURE(kernel);
        for (const f32 ceiling : {8.0f, 64.0f})
        {
            CAPTURE(ceiling);
            // The seeded light's extremes over the four grid parities, and the most green or blue
            // any of them seeded.
            f64 least = std::numeric_limits<f64>::max();
            f64 most = 0.0;
            f64 offHue = 0.0;
            for (const uvec2 texel : {uvec2{32, 32}, uvec2{33, 32}, uvec2{32, 33}, uvec2{33, 33}})
            {
                std::vector<vec3> field = EmptyField();
                At(field, texel.x, texel.y) = vec3(Firefly, 0.0f, 0.0f);
                const LevelSum level = rig.Run(kernel, field, ceiling);
                REQUIRE(level.NonFinite == 0u);
                least = std::min(least, level.Luma());
                most = std::max(most, level.Luma());
                offHue = std::max({offHue, level.Rgb.g, level.Rgb.b});
            }
            // The taps covering one texel carry a total weight of one, each at most the ceiling;
            // the shoulder approaches it closely this far past it. The half-float level stores
            // each texel to a part in a thousand.
            CHECK(most <= ceiling * 1.002);
            CHECK(least >= ceiling * 0.95);
            CHECK(offHue == 0.0);
        }
    }
}

TEST_CASE_FIXTURE(Test::GpuFixture, "bloom bright pass: a NaN or an inf seeds nothing")
{
    AssetManager assets(Context, Tasks, Types);
    BrightPassRig rig(Context, assets);

    // Three poisoned texels and one finite source, far enough apart that no tap covers two of them.
    std::vector<vec3> field = EmptyField();
    At(field, 10, 10) = vec3(std::numeric_limits<f32>::infinity(), 1.0f, 1.0f);
    At(field, 50, 12) = vec3(std::numeric_limits<f32>::quiet_NaN());
    At(field, 12, 50) = vec3(2.0f, -std::numeric_limits<f32>::infinity(), 2.0f);
    constexpr f32 Source = 10.0f;
    At(field, 44, 44) = vec3(Source);

    for (usize kernel = 0; kernel < 2; ++kernel)
    {
        CAPTURE(kernel);
        const LevelSum level = rig.Run(kernel, field, 64.0f);
        CHECK(level.NonFinite == 0u);
        CHECK(level.Luma() == doctest::Approx(Source * 0.25).epsilon(0.01));
    }
}
