// Graphics pipelines and render passes at the backend's own level: one triangle placed from the
// push block into a small target, read back. Each case pins a convention a backend must map rather
// than inherit — where clip space's upper half lands, which winding is the front face, which depth
// wins under the engine's reverse-Z compare, and that a loaded attachment blends with what it held.

#include <array>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ShaderModule.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>
#include <support/GpuContext.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Size = 8;
    constexpr AssetId VertexShaderId{0xB5B397C02C4C929DULL};
    constexpr AssetId FragmentShaderId{0x396E60A65194A327ULL};

    // The probe's push block: three clip-space positions (z is depth) and the colour written.
    struct ProbePush
    {
        std::array<vec4, 3> Positions;
        vec4 Color;
    };

    // A triangle covering the whole target, counter-clockwise as seen on the target (+Y down):
    // top-left, then down the left edge, then along the top.
    constexpr std::array<vec4, 3> Covering{vec4(-1.0f, -1.0f, 0.5f, 0.0f),
                                           vec4(-1.0f, 3.0f, 0.5f, 0.0f),
                                           vec4(3.0f, -1.0f, 0.5f, 0.0f)};

    struct ProbeDraw
    {
        std::array<vec4, 3> Positions = Covering;
        vec4 Color{1.0f};
        f32 Depth = 0.5f;
    };

    struct ProbeTargets
    {
        Ref<Image> Color;
        Ref<ImageView> ColorView;
        Ref<Image> Depth;
        Ref<ImageView> DepthView;
    };

    class Probe
    {
    public:
        explicit Probe(Veng::Test::GpuFixture& fixture)
            : m_Context(fixture.Context), m_Assets(fixture.Context, fixture.Tasks, fixture.Types)
        {
            REQUIRE(m_Assets.Mount(path(TEST_SHADER_PACK)));
            const auto vertex = m_Assets.LoadSync<Shader>(VertexShaderId);
            const auto fragment = m_Assets.LoadSync<Shader>(FragmentShaderId);
            REQUIRE(vertex);
            REQUIRE(fragment);
            m_Vertex = vertex->Get()->Module;
            m_Fragment = fragment->Get()->Module;
            m_Layout = PipelineLayout::Create(
                m_Context, {.Name = "Raster Probe Layout",
                            .PushConstantRanges = {PushConstantRange::Of<ProbePush>(
                                ShaderStage::Vertex | ShaderStage::Fragment)}});
        }

        [[nodiscard]] Ref<GraphicsPipeline> Pipeline(const CullMode cull, const bool depth,
                                                     const BlendState& blend) const
        {
            return GraphicsPipeline::Create(
                m_Context,
                {.Name = "Raster Probe",
                 .ColorAttachments = {{.Format = Format::RGBA8Unorm, .Blend = blend}},
                 .DepthAttachmentFormat = depth ? Format::D32Sfloat : Format::Undefined,
                 .PipelineLayout = m_Layout,
                 .ShaderStages = {{.Stage = ShaderStage::Vertex, .Module = m_Vertex},
                                  {.Stage = ShaderStage::Fragment, .Module = m_Fragment}},
                 .CullMode = cull,
                 .DepthTestEnable = depth,
                 .DepthWriteEnable = depth});
        }

        [[nodiscard]] ProbeTargets Targets(const bool depth) const
        {
            ProbeTargets targets;
            targets.Color = Image::Create(
                m_Context, {.Name = "Raster Probe Target",
                            .Extent = {Size, Size, 1},
                            .Format = Format::RGBA8Unorm,
                            .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc});
            targets.ColorView =
                ImageView::Create(m_Context, {.Name = "Raster Probe View", .Image = targets.Color});
            if (depth)
            {
                targets.Depth = Image::Create(m_Context, {.Name = "Raster Probe Depth",
                                                          .Extent = {Size, Size, 1},
                                                          .Format = Format::D32Sfloat,
                                                          .Usage = ImageUsage::DepthAttachment});
                targets.DepthView = ImageView::Create(
                    m_Context, {.Name = "Raster Probe Depth View", .Image = targets.Depth});
            }
            return targets;
        }

        // Records one pass that clears the target to @p clear (or loads it) and draws @p draws.
        void Render(const ProbeTargets& targets, const Ref<GraphicsPipeline>& pipeline,
                    const std::span<const ProbeDraw> draws, const optional<ClearColor> clear) const
        {
            m_Context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    RenderGraph graph(m_Context);
                    const ResourceId color = graph.Import("Color");
                    const optional<ResourceId> depth =
                        targets.DepthView ? optional(graph.Import("Depth")) : std::nullopt;
                    auto pass = graph.AddPass("Raster Probe");
                    pass.Color({.Resource = color,
                                .Load = clear ? LoadOp::Clear : LoadOp::Load,
                                .Store = StoreOp::Store,
                                .Clear = clear.value_or(ClearColor{})});
                    if (depth)
                    {
                        pass.Depth({.Resource = *depth,
                                    .Load = LoadOp::Clear,
                                    .Store = StoreOp::DontCare,
                                    .Clear = ClearDepth{.Depth = 0.0f}});
                    }
                    pass.Execute(
                        [&](PassContext& ctx)
                        {
                            CommandBuffer& c = ctx.Cmd();
                            c.BindPipeline(pipeline);
                            c.SetViewport({0, 0}, {Size, Size});
                            c.SetScissor({0, 0}, {Size, Size});
                            for (const ProbeDraw& draw : draws)
                            {
                                ProbePush push{.Positions = draw.Positions, .Color = draw.Color};
                                for (vec4& position : push.Positions)
                                {
                                    position.z = draw.Depth;
                                }
                                c.PushConstants(push);
                                c.Draw(3, 1, 0, 0);
                            }
                        });
                    vector<RenderGraph::ImportBinding> bindings{
                        {.Id = color, .View = targets.ColorView}};
                    if (depth)
                    {
                        bindings.push_back({.Id = *depth, .View = targets.DepthView});
                    }
                    graph.Compile()->Execute(cmd, bindings);
                });
        }

    private:
        Context& m_Context;
        AssetManager m_Assets;
        Ref<ShaderModule> m_Vertex;
        Ref<ShaderModule> m_Fragment;
        Ref<PipelineLayout> m_Layout;
    };

    std::array<u8, 4> TexelAt(const vector<u8>& pixels, const u32 x, const u32 y)
    {
        const usize at = ((static_cast<usize>(y) * Size) + x) * 4;
        return {pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
    }

    constexpr std::array<u8, 4> White{255, 255, 255, 255};
    constexpr std::array<u8, 4> Black{0, 0, 0, 255};
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "graphics pipeline: clip space's upper half lands in the target's top rows")
{
    const Probe probe(*this);
    const ProbeTargets targets = probe.Targets(false);
    // A triangle filling clip y in [-1, 0]: the upper half of the target under +Y down.
    const ProbeDraw upper{.Positions = {vec4(-1.0f, -1.0f, 0.5f, 0.0f),
                                        vec4(3.0f, -1.0f, 0.5f, 0.0f),
                                        vec4(-1.0f, 0.0f, 0.5f, 0.0f)}};
    probe.Render(targets, probe.Pipeline(CullMode::None, false, BlendState::Opaque()),
                 std::span(&upper, 1), ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f});

    const vector<u8> pixels = targets.Color->Download();
    CHECK(TexelAt(pixels, 0, 0) == White);
    CHECK(TexelAt(pixels, 0, Size - 1) == Black);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "graphics pipeline: a counter-clockwise triangle is the front face")
{
    const Probe probe(*this);
    const ClearColor black{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f};
    const ProbeDraw covering;

    const ProbeTargets backCulled = probe.Targets(false);
    probe.Render(backCulled, probe.Pipeline(CullMode::Back, false, BlendState::Opaque()),
                 std::span(&covering, 1), black);
    CHECK(TexelAt(backCulled.Color->Download(), Size / 2, Size / 2) == White);

    const ProbeTargets frontCulled = probe.Targets(false);
    probe.Render(frontCulled, probe.Pipeline(CullMode::Front, false, BlendState::Opaque()),
                 std::span(&covering, 1), black);
    CHECK(TexelAt(frontCulled.Color->Download(), Size / 2, Size / 2) == Black);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "graphics pipeline: the nearer surface wins under the reverse-Z compare")
{
    const Probe probe(*this);
    const ProbeTargets targets = probe.Targets(true);
    // Reverse-Z: the larger depth is nearer, so the red draw stays in front of the later green.
    const std::array<ProbeDraw, 2> draws{
        ProbeDraw{.Color = vec4(1.0f, 0.0f, 0.0f, 1.0f), .Depth = 0.75f},
        ProbeDraw{.Color = vec4(0.0f, 1.0f, 0.0f, 1.0f), .Depth = 0.25f}};
    probe.Render(targets, probe.Pipeline(CullMode::None, true, BlendState::Opaque()), draws,
                 ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f});

    CHECK(TexelAt(targets.Color->Download(), Size / 2, Size / 2) ==
          std::array<u8, 4>{255, 0, 0, 255});
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "graphics pipeline: an additive draw blends onto a loaded attachment")
{
    const Probe probe(*this);
    const ProbeTargets targets = probe.Targets(false);
    const ProbeDraw quarter{.Color = vec4(0.25f, 0.0f, 0.0f, 1.0f)};
    // The first pass clears and adds; the second loads what the first stored and adds again.
    probe.Render(targets, probe.Pipeline(CullMode::None, false, BlendState::Additive()),
                 std::span(&quarter, 1), ClearColor{.R = 0.25f, .G = 0.0f, .B = 0.0f, .A = 0.0f});
    probe.Render(targets, probe.Pipeline(CullMode::None, false, BlendState::Additive()),
                 std::span(&quarter, 1), std::nullopt);

    const std::array<u8, 4> texel = TexelAt(targets.Color->Download(), Size / 2, Size / 2);
    // 0.75 of 255 is 191.25; a unorm store rounds it to 191.
    CHECK(texel[0] >= 190);
    CHECK(texel[0] <= 192);
}
