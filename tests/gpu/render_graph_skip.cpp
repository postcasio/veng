// A pass carrying a SkipWhen predicate records nothing on a frame the predicate holds: its
// callback does not run and its attachment keeps what the pass before it wrote, and the passes
// after it still find their resources in the state they declared. On a frame the predicate
// releases, the same compiled graph runs the pass as usual.

#include <array>

#include <doctest/doctest.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "render graph: a skipped pass records nothing and leaves its target as it was")
{
    constexpr u32 Edge = 16;
    RenderGraph graph(Context);
    const ResourceId target = graph.CreateTransient({
        .Name = "Skip Target",
        .Format = Format::RGBA8Unorm,
        .Extent = {Edge, Edge},
        .Usage = ImageUsage::ColorAttachment | ImageUsage::Sampled | ImageUsage::TransferSrc,
    });

    graph.AddPass("clear red")
        .Color({.Resource = target,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 1.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f}})
        .Execute([](PassContext&) {});

    bool skip = true;
    u32 recorded = 0;
    graph.AddPass("skippable")
        .Color({.Resource = target, .Load = LoadOp::Load, .Store = StoreOp::Store})
        .SkipWhen([&skip] { return skip; })
        .Execute([&recorded](PassContext&) { recorded++; });

    // A reader after the skipped pass: its declared sample still finds the target transitioned.
    u32 sampled = 0;
    graph.AddPass("sample").Sample(target).Execute([&sampled](PassContext&) { sampled++; });

    const auto compiled = graph.Compile();
    const Ref<Image> image = compiled->ResolvedImage(target);
    REQUIRE(image != nullptr);

    Context.ImmediateCommands([&](CommandBuffer& cmd) { compiled->Execute(cmd); });
    CHECK(recorded == 0u);
    CHECK(sampled == 1u);
    constexpr std::array<u8, 4> red = {255, 0, 0, 255};
    CHECK(Test::PixelsMatch(image->Download(), red));

    skip = false;
    Context.ImmediateCommands([&](CommandBuffer& cmd) { compiled->Execute(cmd); });
    CHECK(recorded == 1u);
    CHECK(sampled == 2u);
    CHECK(Test::PixelsMatch(image->Download(), red));
}
