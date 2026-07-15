// VolumeField build + readback: the CPU->3D-texture upload path exercised end to end through the
// resource factory for the first time. A small RGBA16F field with a known per-voxel byte pattern
// is built, its Type3D image downloaded (folding extent.z), and asserted byte-identical to the
// source — the depth axis is the variable, so a dropped or mis-strided z slice fails here. Both
// factory arms are covered: the blocking BuildSync and the async Build (which resolves through the
// continuation pump). A buffer->image->buffer copy is a bitwise transfer, so an exact byte compare
// is the right assertion regardless of how the half-float bit patterns decode.

#include <cstring>
#include <vector>

#include <doctest/doctest.h>

#include <Veng/Math/AABB.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Renderer/VolumeField.h>
#include <Veng/Task/TaskSystem.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // A deterministic byte pattern over the full x-major extent. Every byte is a function of its
    // linear position, so any dropped or reordered z slice changes the bytes and fails the compare.
    std::vector<u8> KnownVoxels(usize byteSize)
    {
        std::vector<u8> voxels(byteSize);
        for (usize i = 0; i < byteSize; ++i)
        {
            voxels[i] = static_cast<u8>((i * 131u + 17u) & 0xFFu);
        }
        return voxels;
    }

    // Drains the worker pool and pumps the main-thread continuation so an async Build's Then runs.
    void Pump(TaskSystem& tasks)
    {
        tasks.WaitForAll();
        tasks.PumpMainThread();
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "VolumeField::BuildSync: a 3D field uploads and reads back byte-identical")
{
    constexpr u32 W = 8;
    constexpr u32 H = 8;
    constexpr u32 D = 8;

    VolumeFieldData data;
    data.Name = "Sync Volume";
    data.Resolution = {W, H, D};
    data.Format = Format::RGBA16Sfloat;
    data.Bounds = AABB{.Min = vec3(-2.0f, -1.0f, 0.5f), .Max = vec3(2.0f, 3.0f, 4.5f)};

    const std::vector<u8> voxels = KnownVoxels(data.ExpectedByteSize());
    data.Voxels = voxels;
    REQUIRE(data.IsValid());

    const Ref<VolumeField> field = VolumeField::BuildSync(Context, data);
    REQUIRE(field != nullptr);

    // The resource carries the bounds, resolution, and format verbatim, and exposes a live view
    // and sampler (no bindless registration, complete on return).
    CHECK(field->GetResolution() == uvec3(W, H, D));
    CHECK(field->GetFormat() == Format::RGBA16Sfloat);
    CHECK(field->GetBounds().Min == data.Bounds.Min);
    CHECK(field->GetBounds().Max == data.Bounds.Max);
    REQUIRE(field->GetImage() != nullptr);
    REQUIRE(field->GetImageView() != nullptr);
    REQUIRE(field->GetSampler() != nullptr);
    CHECK(field->GetImage()->GetType() == ImageType::Type3D);
    CHECK(field->GetImage()->GetExtent() == uvec3(W, H, D));

    // Download folds extent.z: the full volume comes back, byte-identical to what was uploaded.
    const std::vector<u8> back = field->GetImage()->Download();
    REQUIRE(back.size() == voxels.size());
    CHECK(std::memcmp(back.data(), voxels.data(), voxels.size()) == 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "VolumeField::Build: the async factory resolves a byte-identical field")
{
    constexpr u32 W = 8;
    constexpr u32 H = 8;
    constexpr u32 D = 8;

    // The async Build uploads through the transfer queue, which needs the per-worker transfer
    // pools the Application wires up; the bare GpuFixture leaves them uninitialized.
    Context.InitializeTransferPools(Tasks);

    VolumeFieldData data;
    data.Name = "Async Volume";
    data.Resolution = {W, H, D};
    data.Format = Format::RGBA16Sfloat;
    data.Bounds = AABB{.Min = vec3(0.0f), .Max = vec3(1.0f, 2.0f, 3.0f)};

    const std::vector<u8> voxels = KnownVoxels(data.ExpectedByteSize());
    data.Voxels = voxels;
    REQUIRE(data.IsValid());

    // Build runs on a worker; the result lands through the continuation pump, so it is not resident
    // the instant Build returns.
    Task<Ref<VolumeField>> task = VolumeField::Build(Context, Tasks, data);
    Ref<VolumeField> field;
    task.Then(
        [&field](Result<Ref<VolumeField>> result)
        {
            REQUIRE(result.has_value());
            field = *result;
        });

    Pump(Tasks);
    REQUIRE(field != nullptr);

    CHECK(field->GetResolution() == uvec3(W, H, D));
    CHECK(field->GetBounds().Min == data.Bounds.Min);
    CHECK(field->GetBounds().Max == data.Bounds.Max);
    CHECK(field->GetImage()->GetType() == ImageType::Type3D);

    const std::vector<u8> back = field->GetImage()->Download();
    REQUIRE(back.size() == voxels.size());
    CHECK(std::memcmp(back.data(), voxels.data(), voxels.size()) == 0);
}
