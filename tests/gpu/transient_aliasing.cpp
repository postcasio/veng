// Transient-aliasing cases: prove the compiler shares one backing image between
// two non-overlapping same-key transients (AssignTransientSlots, wired into
// RenderGraph::Compile), and that the per-frame barrier schedule serializes the
// reuse so the shared storage is not corrupted. A negative case confirms
// overlapping transients keep distinct images.

#include <array>

#include <doctest/doctest.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Width = 64;
    constexpr u32 Height = 64;

    // The size class both transients in the sharing cases declare.
    TransientDesc TransientColor(const string& name)
    {
        return {
            .Name = name,
            .Format = Format::RGBA8Unorm,
            .Extent = {Width, Height},
            .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc,
        };
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "transient aliasing: disjoint same-key transients share one image, reuse serialized")
{
    RenderGraph graph(Context);

    // A is used only in pass 0; B only in pass 1. Their live ranges (0..0 and
    // 1..1) do not overlap and the size class matches, so the compiler collapses
    // them onto one slot.
    const ResourceId a = graph.CreateTransient(TransientColor("Transient A"));
    const ResourceId b = graph.CreateTransient(TransientColor("Transient B"));

    graph.AddPass("clear A green")
        .Color({.Resource = a,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 1.0f, .B = 0.0f, .A = 1.0f}})
        .Execute([](PassContext&) {});

    graph.AddPass("clear B blue")
        .Color({.Resource = b,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 1.0f, .A = 1.0f}})
        .Execute([](PassContext&) {});

    const auto compiled = graph.Compile();

    // Slot sharing happened: both transients resolved to the same backing image.
    const Ref<Image> imageA = compiled->ResolvedImage(a);
    const Ref<Image> imageB = compiled->ResolvedImage(b);
    REQUIRE(imageA != nullptr);
    REQUIRE(imageB != nullptr);
    CHECK(imageA.get() == imageB.get());

    Context.ImmediateCommands([&](CommandBuffer& cmd) { compiled->Execute(cmd); });

    // The shared storage holds B's clear (blue) — pass 1 ran after pass 0 and
    // fully overwrote it. The reuse serialized cleanly: no green bleed, no
    // garbage.
    constexpr std::array<u8, 4> blue = {0, 0, 255, 255};
    const vector<u8> pixels = imageB->Download();
    REQUIRE(pixels.size() == static_cast<size_t>(Width) * Height * 4);
    CHECK(Test::PixelsMatch(pixels, blue));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "transient aliasing: overlapping transients get distinct images")
{
    RenderGraph graph(Context);

    // Both transients are written in pass 0, so their live ranges overlap and
    // they cannot share a slot.
    const ResourceId a = graph.CreateTransient(TransientColor("Overlap A"));
    const ResourceId b = graph.CreateTransient(TransientColor("Overlap B"));

    graph.AddPass("clear both")
        .Color({.Resource = a,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 1.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f}})
        .Color({.Resource = b,
                .Load = LoadOp::Clear,
                .Store = StoreOp::Store,
                .Clear = ClearColor{.R = 0.0f, .G = 1.0f, .B = 0.0f, .A = 1.0f}})
        .Execute([](PassContext&) {});

    const auto compiled = graph.Compile();

    const Ref<Image> imageA = compiled->ResolvedImage(a);
    const Ref<Image> imageB = compiled->ResolvedImage(b);
    REQUIRE(imageA != nullptr);
    REQUIRE(imageB != nullptr);
    CHECK(imageA.get() != imageB.get());

    Context.ImmediateCommands([&](CommandBuffer& cmd) { compiled->Execute(cmd); });

    // Each kept its own storage, so each holds its own clear colour.
    constexpr std::array<u8, 4> red = {255, 0, 0, 255};
    constexpr std::array<u8, 4> green = {0, 255, 0, 255};
    CHECK(Test::PixelsMatch(imageA->Download(), red));
    CHECK(Test::PixelsMatch(imageB->Download(), green));
}
