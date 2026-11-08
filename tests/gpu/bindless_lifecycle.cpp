// BindlessRegistry slot lifecycle, through the public registry and frame API alone: a released
// slot is not reused while a frame in flight may still read it and is reclaimed once that frame
// slot retires; a context that only submits one-off work reclaims once nothing is in flight, and a
// recording nested in a frame reclaims nothing; textures asking for one sampling description share
// one sampler slot; the global anisotropy rebuilds only the samplers that opted in; and each typed
// registry takes only its own view type. Backend-neutral: the slot logic is the device-free
// registry's.

#include <array>
#include <string>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Size = 4;

    // One slot in each slot-keyed array a release defers, plus a material range, all released at
    // once — the drops a renderer resize or teardown makes between frames.
    struct SlotSet
    {
        TextureHandle Texture;
        SamplerHandle Sampler;
        StorageBufferHandle StorageBuffer;
        MaterialHandle Material;
    };

    SlotSet RegisterSlotSet(Context& context)
    {
        BindlessRegistry& bindless = context.GetBindlessRegistry();
        const Ref<Image> image = Image::Create(context, {
                                                            .Name = "Reclaim Texture",
                                                            .Extent = {Size, Size, 1},
                                                            .Format = Format::RGBA8Unorm,
                                                            .Usage = ImageUsage::Sampled,
                                                        });
        const Ref<Sampler> sampler = Sampler::Create(context, {.Name = "Reclaim Sampler"});
        const Ref<Buffer> buffer = Buffer::Create(context, {
                                                               .Name = "Reclaim Buffer",
                                                               .Size = 256,
                                                               .Usage = BufferUsage::Storage,
                                                           });
        constexpr std::array<std::byte, 16> block{};
        return SlotSet{
            .Texture = bindless.Register(
                ImageView::Create(context, {.Name = "Reclaim Texture View", .Image = image})),
            .Sampler = bindless.Register(sampler),
            .StorageBuffer = bindless.Register(buffer),
            .Material = bindless.RegisterMaterial(block),
        };
    }

    void ReleaseSlotSet(const Context& context, const SlotSet& set)
    {
        BindlessRegistry& bindless = context.GetBindlessRegistry();
        bindless.Release(set.Texture);
        bindless.Release(set.Sampler);
        bindless.Release(set.StorageBuffer);
        bindless.Release(set.Material);
    }

    bool SameFreeSlots(const BindlessCapacity& a, const BindlessCapacity& b)
    {
        return a.Textures == b.Textures && a.Samplers == b.Samplers &&
               a.StorageBuffers == b.StorageBuffers && a.Materials == b.Materials &&
               a.MaterialBlocks == b.MaterialBlocks;
    }

}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "bindless registry: released slots are not reused until "
                                          "they cycle through every frame in flight")
{
    auto& bindless = Context.GetBindlessRegistry();

    const auto MakeView = [&](string_view name)
    {
        const auto image = Image::Create(Context, {
                                                      .Name = string(name),
                                                      .Extent = {Size, Size, 1},
                                                      .Format = Format::RGBA8Unorm,
                                                      .Usage = ImageUsage::Sampled,
                                                  });

        return ImageView::Create(Context, {.Name = string(name) + " View", .Image = image});
    };

    const auto viewA = MakeView("Slot A");
    const auto viewB = MakeView("Slot B");
    const auto viewC = MakeView("Slot C");
    const auto viewD = MakeView("Slot D");

    const TextureHandle handleA = bindless.Register(viewA);
    const TextureHandle handleB = bindless.Register(viewB);

    bindless.Release(handleA);

    // The slot freed by Release(handleA) must not be handed out immediately —
    // it's still potentially sampled by in-flight frames.
    const TextureHandle handleC = bindless.Register(viewC);
    CHECK(handleC.Index != handleA.Index);

    // Cycle through every frame-in-flight index so AcquireNextFrame() (called
    // from BeginFrame/EndFrame) processes handleA's deferred release.
    const u32 framesInFlight = Context.GetMaxFramesInFlight();
    for (u32 i = 0; i < framesInFlight + 1; i++)
    {
        Context.BeginFrame();
        Context.EndFrame();
    }

    const TextureHandle handleD = bindless.Register(viewD);
    CHECK(handleD.Index == handleA.Index);

    bindless.Release(handleB);
    bindless.Release(handleC);
    bindless.Release(handleD);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless registry: textures sharing a sampling description share one slot")
{
    AssetManager assets(Context, Tasks, Types);
    const BindlessRegistry& bindless = Context.GetBindlessRegistry();

    constexpr std::array<u8, 4> pixel = {255, 255, 255, 255};

    // Every texture but the last asks for the same sampling and a different debug name, so the
    // name plays no part in what is shared.
    const auto BuildTexture = [&](const string& name, const AddressMode address)
    {
        return assets.BuildSync<Texture>(TextureData{
            .Name = name,
            .Extent = {1, 1},
            .Format = Format::RGBA8Unorm,
            .Pixels = pixel,
            .Sampler =
                {
                    .AddressModeU = address,
                    .AddressModeV = address,
                    .AddressModeW = address,
                },
        });
    };

    // The baseline is taken after one build rather than before it: the first texture in a run is
    // what mints the shared sampler, so the readings bracket what a further texture costs instead
    // of asserting the first one away.
    vector<AssetHandle<Texture>> textures;
    textures.push_back(BuildTexture("Shared Sampler Warm-up", AddressMode::ClampToEdge));

    const BindlessCapacity before = bindless.GetFreeSlots();
    REQUIRE(before.Samplers > 0);

    constexpr u32 Count = 32;
    for (u32 i = 0; i < Count; i++)
    {
        textures.push_back(
            BuildTexture("Shared Sampler " + std::to_string(i), AddressMode::ClampToEdge));
    }

    const BindlessCapacity after = bindless.GetFreeSlots();

    // A sampler is pure state, so an identical description is the same sampler: the whole batch
    // reads through the slot the warm-up took. Each texture still holds an image slot of its own,
    // which is what makes the sampler reading a statement about sharing rather than about nothing
    // having been built.
    CHECK(after.Samplers == before.Samplers);
    CHECK(after.Textures == before.Textures - Count);

    const u32 sharedSlot = textures.front().Get()->GetSamplerHandle().Index;
    for (const AssetHandle<Texture>& texture : textures)
    {
        REQUIRE(texture.Get() != nullptr);
        CHECK(texture.Get()->GetSamplerHandle().Index == sharedSlot);
    }

    // A description that differs in a field the GPU acts on takes a slot of its own — the cache
    // matches on the sampling, not on the asking.
    const AssetHandle<Texture> repeating = BuildTexture("Repeating", AddressMode::Repeat);
    CHECK(bindless.GetFreeSlots().Samplers == before.Samplers - 1);
    CHECK(repeating.Get()->GetSamplerHandle().Index != sharedSlot);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless registry: global anisotropy rebuilds only the "
                  "opted-in samplers, in place, and clamps to the device max")
{
    // The confinement property: SetGlobalAnisotropy reaches a scene-texture description (one that
    // opted into HonorGlobalAnisotropy) and no other — an aniso-off description and a default-on one
    // that relied on the SamplerInfo default are left exactly as authored. The rebuild is in place
    // (same slot, no handle churn), which the validation gate over this binary proves is properly
    // synchronized (the WaitIdle guarding the in-flight-slot rewrite).
    auto& bindless = Context.GetBindlessRegistry();

    const SamplerInfo sceneInfo{.Name = "Scene Texture", .HonorGlobalAnisotropy = true};
    const SamplerInfo defaultInfo{.Name = "Internal Default"};
    const SamplerInfo offInfo{.Name = "Aniso Off", .AnisotropyEnabled = false};

    const SharedSampler scene = bindless.AcquireSampler(sceneInfo);
    const SharedSampler defaulted = bindless.AcquireSampler(defaultInfo);
    const SharedSampler off = bindless.AcquireSampler(offInfo);

    // The distinct descriptions take distinct slots — the opted-in flag is part of the cache key, so
    // the scene and default descriptions do not share despite matching in every other field.
    CHECK(scene.Handle.Index != defaulted.Handle.Index);

    // The default global state reproduces today: the opted-in sampler filters at the shared 8x.
    CHECK(scene.Sampler->GetInfo().AnisotropyEnabled);
    CHECK(scene.Sampler->GetInfo().MaxAnisotropy == doctest::Approx(DefaultMaxAnisotropy));

    const Sampler* const sceneBefore = scene.Sampler.get();
    const Sampler* const defaultBefore = defaulted.Sampler.get();
    const Sampler* const offBefore = off.Sampler.get();

    // Change the global value to a distinct, in-range one (the device max is >= 16 with the feature).
    bindless.SetGlobalAnisotropy(true, 2.0f);

    // Re-acquiring returns the same cache entries (same author descriptions).
    const SharedSampler sceneAfter = bindless.AcquireSampler(sceneInfo);
    const SharedSampler defaultAfter = bindless.AcquireSampler(defaultInfo);
    const SharedSampler offAfter = bindless.AcquireSampler(offInfo);

    // The opted-in sampler kept its slot but was rebuilt to the new value — the in-place rewrite.
    CHECK(sceneAfter.Handle.Index == scene.Handle.Index);
    CHECK(sceneAfter.Sampler.get() != sceneBefore);
    CHECK(sceneAfter.Sampler->GetInfo().MaxAnisotropy == doctest::Approx(2.0f));

    // Confinement: the default-on and aniso-off samplers are untouched — same object, same value.
    CHECK(defaultAfter.Sampler.get() == defaultBefore);
    CHECK(defaultAfter.Sampler->GetInfo().MaxAnisotropy == doctest::Approx(DefaultMaxAnisotropy));
    CHECK(offAfter.Sampler.get() == offBefore);
    CHECK_FALSE(offAfter.Sampler->GetInfo().AnisotropyEnabled);

    // A value above the device max clamps to it (the correctness fix the unclamped 8x skated on).
    const f32 deviceMax = Context.GetMaxSamplerAnisotropy();
    bindless.SetGlobalAnisotropy(true, deviceMax * 4.0f);
    CHECK(bindless.AcquireSampler(sceneInfo).Sampler->GetInfo().MaxAnisotropy ==
          doctest::Approx(deviceMax));

    // An unchanged apply rebuilds nothing — the dirty-compare early-out.
    const Sampler* const clampedScene = bindless.AcquireSampler(sceneInfo).Sampler.get();
    bindless.SetGlobalAnisotropy(true, deviceMax * 4.0f);
    CHECK(bindless.AcquireSampler(sceneInfo).Sampler.get() == clampedScene);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless registry: a typed registry holds exactly its own view type")
{
    // The homogeneity invariant is the whole point of the typed sets: a 3D view registers into the
    // volume set and a cube view into the cube set, each leaving the 2D array untouched — which is
    // what keeps a non-2D descriptor out of set 0's Metal argument buffer. Proved by the free-slot
    // accounting: registering into one set decrements that set's count and no other.
    BindlessRegistry& bindless = Context.GetBindlessRegistry();

    const Ref<Image> volumeImage =
        Image::Create(Context, {
                                   .Name = "Homogeneity Volume",
                                   .Extent = {4, 4, 4},
                                   .Format = Format::RGBA16Sfloat,
                                   .Type = ImageType::Type3D,
                                   .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                               });
    const Ref<ImageView> volumeView =
        ImageView::Create(Context, {.Name = "Homogeneity Volume View",
                                    .Image = volumeImage,
                                    .ViewType = ImageViewType::Type3D});

    const Ref<Image> cubeImage =
        Image::Create(Context, {
                                   .Name = "Homogeneity Cube",
                                   .Extent = {4, 4, 1},
                                   .Layers = 6,
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                               });
    const Ref<ImageView> cubeView = ImageView::Create(Context, {.Name = "Homogeneity Cube View",
                                                                .Image = cubeImage,
                                                                .ViewType = ImageViewType::Cube,
                                                                .ArrayLayers = 6});

    const BindlessCapacity before = bindless.GetFreeSlots();

    const VolumeHandle volume = bindless.RegisterVolume(volumeView);
    REQUIRE(volume.IsValid());
    const BindlessCapacity afterVolume = bindless.GetFreeSlots();
    CHECK(afterVolume.Volumes == before.Volumes - 1);
    CHECK(afterVolume.Textures == before.Textures); // the 2D array is untouched
    CHECK(afterVolume.Cubes == before.Cubes);

    const CubeHandle cube = bindless.RegisterCube(cubeView);
    REQUIRE(cube.IsValid());
    const BindlessCapacity afterCube = bindless.GetFreeSlots();
    CHECK(afterCube.Cubes == before.Cubes - 1);
    CHECK(afterCube.Volumes == afterVolume.Volumes); // the volume array is untouched
    CHECK(afterCube.Textures == before.Textures);

    bindless.Release(volume);
    bindless.Release(cube);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless registry: a context that never begins a frame reclaims released slots")
{
    const BindlessRegistry& bindless = Context.GetBindlessRegistry();
    const BindlessCapacity before = bindless.GetFreeSlots();

    ReleaseSlotSet(Context, RegisterSlotSet(Context));
    CHECK_FALSE(SameFreeSlots(bindless.GetFreeSlots(), before));

    // The recording completes before it returns and no frame was ever submitted, so nothing can
    // still read the released slots.
    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(SameFreeSlots(bindless.GetFreeSlots(), before));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "bindless registry: an immediate submit inside a frame reclaims nothing")
{
    // The frame's own command buffer is open and unsubmitted, and may already have recorded a draw
    // through a slot released after it; only that frame's fence can retire the slot.
    const BindlessRegistry& bindless = Context.GetBindlessRegistry();

    Context.BeginFrame();
    const BindlessCapacity before = bindless.GetFreeSlots();
    ReleaseSlotSet(Context, RegisterSlotSet(Context));
    const BindlessCapacity released = bindless.GetFreeSlots();
    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(SameFreeSlots(bindless.GetFreeSlots(), released));
    Context.EndFrame();

    Context.ImmediateCommands([](CommandBuffer&) {});
    Context.WaitIdle();
    Context.ImmediateCommands([](CommandBuffer&) {});
    CHECK(SameFreeSlots(bindless.GetFreeSlots(), before));
}
