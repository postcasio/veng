// A sphere terrain's translucent level layer, device-free: which surfaces a terrain draws with which
// materials, and the set a translucent layer's stages read the terrain at, which must collide with
// none a translucent pipeline already binds.

#include <array>

#include <doctest/doctest.h>

#include <Veng/Asset/Material.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>

#include <Renderer/Passes/TranslucentScenePass.h>
#include <Renderer/SphereTerrainDraw.h>
#include <Renderer/SphereTerrainLayout.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("sphere terrain material gate: a translucent layer is drawn, a translucent ground is not")
{
    using enum SphereTerrainMaterialVerdict;
    CHECK(CheckSphereTerrainMaterial(MaterialDomain::Surface, true, false) == Opaque);
    CHECK(CheckSphereTerrainMaterial(MaterialDomain::Surface, true, true) == Opaque);
    CHECK(CheckSphereTerrainMaterial(MaterialDomain::Translucent, true, true) == Translucent);
    CHECK(CheckSphereTerrainMaterial(MaterialDomain::Translucent, true, false) ==
          TranslucentGround);
    // Neither domain draws without the declaration, and no other domain draws at all.
    CHECK(CheckSphereTerrainMaterial(MaterialDomain::Surface, false, false) == NotTerrain);
    CHECK(CheckSphereTerrainMaterial(MaterialDomain::Translucent, false, true) == NotTerrain);
    for (const MaterialDomain domain :
         {MaterialDomain::PostProcess, MaterialDomain::Sky, MaterialDomain::GuiFill})
    {
        CHECK(CheckSphereTerrainMaterial(domain, true, true) == WrongDomain);
    }
}

TEST_CASE("sphere terrain sets: a translucent layer's terrain set collides with no translucent set")
{
    // A translucent pipeline binds the bindless sets, the per-draw data, then forward lighting's IBL
    // and shadow sets; the opaque terrain's set 4 is the IBL's there, so a layer's terrain set must
    // lie past all of them, and the whole layout must stay contiguous and bindable at once.
    constexpr u32 drawData = BindlessRegistry::FirstUserSet;
    constexpr std::array<u32, 4> sets = {drawData, ForwardLightingIblSet, ForwardLightingShadowSet,
                                         TranslucentSphereTerrainSet};
    for (usize i = 0; i < sets.size(); i++)
    {
        CHECK(sets[i] == drawData + i);
    }
    CHECK(SphereTerrainSet == ForwardLightingIblSet);
    CHECK(TranslucentSphereTerrainSet != SphereTerrainSet);
    CHECK(TranslucentSphereTerrainSet != SurfaceShadowMapSet);
    CHECK(TranslucentSphereTerrainSet < CommandBuffer::MaxBoundDescriptorSets);

    // The terrain set carries every channel, the records and the lists, and nothing past them.
    for (u32 channel = 0; channel < MaxSphereTerrainChannels; channel++)
    {
        CHECK(IsSphereTerrainBinding(channel, DescriptorType::SampledImage));
    }
    CHECK(IsSphereTerrainBinding(SphereTerrainRecordsBinding, DescriptorType::StorageBuffer));
    CHECK(IsSphereTerrainBinding(SphereTerrainListBinding, DescriptorType::StorageBuffer));
    CHECK_FALSE(
        IsSphereTerrainBinding(SphereTerrainListBinding + 1, DescriptorType::StorageBuffer));
    CHECK_FALSE(IsSphereTerrainBinding(0, DescriptorType::StorageBuffer));
}
