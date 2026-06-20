// Pure-logic unit cases: the engine's device-free logic. No Context, no
// Vulkan symbol touched — these run with no ICD present.

#include <doctest/doctest.h>

#include <span>
#include <expected>

#include <Veng/Result.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VertexBufferLayout.h>

using namespace Veng;
using namespace Veng::Renderer;

// --- Result<T> / VoidResult -------------------------------------------------
// Thin aliases over std::expected; this is a contract/smoke check of veng's use
// pattern (truthiness, value, error), not a re-test of the standard library.

TEST_CASE("Result<T> carries a success value")
{
    Result<int> r = 42;

    CHECK(r.has_value());
    CHECK(static_cast<bool>(r));
    CHECK(*r == 42);
    CHECK(r.value() == 42);
}

TEST_CASE("Result<T> carries an error string")
{
    Result<int> r = std::unexpected("boom");

    CHECK_FALSE(r.has_value());
    CHECK_FALSE(static_cast<bool>(r));
    CHECK(r.error() == "boom");
}

TEST_CASE("VoidResult success and error paths")
{
    VoidResult ok;
    CHECK(ok.has_value());
    CHECK(static_cast<bool>(ok));

    VoidResult err = std::unexpected("nope");
    CHECK_FALSE(err.has_value());
    CHECK(err.error() == "nope");
}

// --- VertexBufferLayout -----------------------------------------------------
// Offsets/stride/float-count are computed in the ctor and are device-free.

TEST_CASE("VertexBufferLayout computes offsets, stride and float count")
{
    const VertexBufferLayout layout = {
        {Format::RG32Sfloat, "uv"},
        {Format::RGB32Sfloat, "position"},
    };

    const auto& elements = layout.GetElements();
    REQUIRE(elements.size() == 2);

    // Sizes from the float component counts (2*4, 3*4).
    CHECK(elements[0].Size == 8);
    CHECK(elements[1].Size == 12);

    // Offset is the running sum of prior element sizes; first is 0.
    CHECK(elements[0].Offset == 0);
    CHECK(elements[1].Offset == 8);

    CHECK(layout.GetStride() == 20);    // 8 + 12
    CHECK(layout.GetFloatCount() == 5); // 2 + 3
}

TEST_CASE("VertexBufferLayout single element")
{
    const VertexBufferLayout layout = {
        {Format::R32Sfloat, "scalar"},
    };

    REQUIRE(layout.GetElements().size() == 1);
    CHECK(layout.GetElements()[0].Offset == 0);
    CHECK(layout.GetStride() == 4);
    CHECK(layout.GetFloatCount() == 1);
}

TEST_CASE("VertexBufferLayout: initializer_list and vector ctors agree")
{
    // Guards the two duplicated ctor bodies from drifting.
    const VertexBufferLayout fromList = {
        {Format::RGBA32Sfloat, "color"},
        {Format::RG32Sfloat, "uv"},
        {Format::R32Sfloat, "weight"},
    };

    const vector<VertexBufferElement> elems = {
        {Format::RGBA32Sfloat, "color"},
        {Format::RG32Sfloat, "uv"},
        {Format::R32Sfloat, "weight"},
    };
    const VertexBufferLayout fromVector(elems);

    REQUIRE(fromList.GetElements().size() == fromVector.GetElements().size());
    CHECK(fromList.GetStride() == fromVector.GetStride());
    CHECK(fromList.GetFloatCount() == fromVector.GetFloatCount());

    for (usize i = 0; i < fromList.GetElements().size(); ++i)
    {
        CHECK(fromList.GetElements()[i].Offset == fromVector.GetElements()[i].Offset);
        CHECK(fromList.GetElements()[i].Size == fromVector.GetElements()[i].Size);
    }

    // Many-element layout: stride is the running total (16 + 8 + 4), floats 4+2+1.
    CHECK(fromList.GetStride() == 28);
    CHECK(fromList.GetFloatCount() == 7);
}
