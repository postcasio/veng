// The MaterialParams drift guard. The engine-side Renderer::MaterialParams mirror
// (Veng/Renderer/MaterialParams.h) carries static_asserts on its sizeof and each
// field offsetof against the documented metallic-roughness layout the byte-identical
// Slang copies (core material.slang + the example/test material_data.slang) and the
// cooker's reflected offset-patching all agree on. Including the header here makes
// those static_asserts part of this translation unit, so a drift between the header
// and the documented layout is a build error; the runtime CHECKs re-state the same
// offsets so the case is also a visible assertion.

#include <doctest/doctest.h>

#include <Veng/Renderer/MaterialParams.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("MaterialParams: the metallic-roughness block matches the documented layout")
{
    CHECK(sizeof(MaterialParams) == 64);

    CHECK(offsetof(MaterialParams, BaseColor) == 0);
    CHECK(offsetof(MaterialParams, BaseColorSampler) == 4);
    CHECK(offsetof(MaterialParams, ORM) == 8);
    CHECK(offsetof(MaterialParams, ORMSampler) == 12);
    CHECK(offsetof(MaterialParams, BaseColorFactor) == 16);
    CHECK(offsetof(MaterialParams, EmissiveFactor) == 32);
    CHECK(offsetof(MaterialParams, MetallicFactor) == 48);
    CHECK(offsetof(MaterialParams, RoughnessFactor) == 52);
    CHECK(offsetof(MaterialParams, OcclusionStrength) == 56);
    CHECK(offsetof(MaterialParams, Pad0) == 60);
}
