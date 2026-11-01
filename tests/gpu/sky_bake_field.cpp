// A layered sky bake beginning with a field. The field material writes three targets, each a
// known, smooth function of the view direction, at a quarter of the face size; a composite material
// reads one back into the cube — bicubically, by mip, or as its solid-angle mean — so every read is
// checked against the function it reconstructs. The re-composite, supersede and per-phase cost
// cases drive the amortized job pump by pump and read its tile counters.
//
// Gated on GPU_GBUFFER_FIXTURE_DIR (the in-process cook of the fixture materials).

#include <array>
#include <cmath>

#include <doctest/doctest.h>

#include <gpu/fixture.h>

#ifdef GPU_GBUFFER_FIXTURE_DIR

#include <filesystem>
#include "support/TempPath.h"
#include "support/TestCook.h"

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/BakedSkyCube.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GeneratedTextureService.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Scene/BuiltinTypes.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr AssetId FieldInstanceId{0x312B22B1AEBB23E3ULL};
    constexpr AssetId ReadInstanceId{0xB36D5B22E64BBCE1ULL};

    // The display face, and the divisor the field bakes at: a 16² field under a 64² cube, four
    // display texels to a field texel.
    constexpr u32 FaceSize = 64;
    constexpr u32 FieldDivisor = 4;
    constexpr u32 FieldFaceSize = FaceSize / FieldDivisor;
    // Two field tiles a side, so the field phase is 6 × 4 ticks.
    constexpr u32 FieldTilePixels = 8;
    constexpr u32 FieldTicks = BakedSkyCube::CubeFaces * 4;

    // The direction through face coordinates uv of a cube face, in the bake's face convention.
    vec3 FaceDirection(const u32 face, const vec2 uv)
    {
        const vec2 st = uv * 2.0f - 1.0f;
        switch (face)
        {
        case 0:
            return glm::normalize(vec3(1.0f, -st.y, -st.x));
        case 1:
            return glm::normalize(vec3(-1.0f, -st.y, st.x));
        case 2:
            return glm::normalize(vec3(st.x, 1.0f, st.y));
        case 3:
            return glm::normalize(vec3(st.x, -1.0f, -st.y));
        case 4:
            return glm::normalize(vec3(st.x, -st.y, 1.0f));
        default:
            return glm::normalize(vec3(-st.x, -st.y, -1.0f));
        }
    }

    // The three functions the field material writes (sky_field.frag), alpha included.
    vec4 FieldFunction(const u32 target, const vec3 d)
    {
        switch (target)
        {
        case 0:
            return {0.5f + 0.5f * d, 1.0f};
        case 1:
            return {d.x * d.y, d.y * d.z, d.z * d.x, 0.5f};
        default:
        {
            const vec3 d2 = d * d;
            return {d2 * d2, 1.0f};
        }
        }
    }

    // The solid angle of the face rectangle from its centre to (x, y), in [-1, 1] face coordinates.
    f64 CornerSolidAngle(const f64 x, const f64 y)
    {
        return std::atan2(x * y, std::sqrt(x * x + y * y + 1.0));
    }

    // The solid-angle mean over the sphere of a target sampled at the field's texel centres — what
    // the bake's reduction computes, without the half-float storage.
    vec4 SolidAngleMean(const u32 target)
    {
        glm::dvec4 sum(0.0);
        const f64 texel = 2.0 / FieldFaceSize;
        for (u32 face = 0; face < BakedSkyCube::CubeFaces; ++face)
        {
            for (u32 y = 0; y < FieldFaceSize; ++y)
            {
                for (u32 x = 0; x < FieldFaceSize; ++x)
                {
                    const f64 x0 = x * texel - 1.0;
                    const f64 y0 = y * texel - 1.0;
                    const f64 omega = CornerSolidAngle(x0 + texel, y0 + texel) -
                                      CornerSolidAngle(x0, y0 + texel) -
                                      CornerSolidAngle(x0 + texel, y0) + CornerSolidAngle(x0, y0);
                    const vec2 uv((static_cast<f32>(x) + 0.5f) / FieldFaceSize,
                                  (static_cast<f32>(y) + 0.5f) / FieldFaceSize);
                    sum += omega * glm::dvec4(FieldFunction(target, FaceDirection(face, uv)));
                }
            }
        }
        return vec4(sum / (4.0 * glm::pi<f64>()));
    }

    // All six faces of the displayed cube's mip 0, as RGBA16F, layer-major.
    vector<u8> DownloadCube(Context& context, BakedSkyCube& bake)
    {
        const usize faceBytes = static_cast<usize>(FaceSize) * FaceSize * 8;
        const Ref<Buffer> staging = Buffer::Create(context, {
                                                                .Name = "Sky Field Readback",
                                                                .Size = faceBytes * 6,
                                                                .Usage = BufferUsage::TransferDst,
                                                            });
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                cmd.PrepareForAccess(bake.GetCubeView(), AccessKind::TransferSrc);
                const vk::BufferImageCopy region{
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 6},
                    .imageExtent = {.width = FaceSize, .height = FaceSize, .depth = 1},
                };
                GetVkCommandBuffer(cmd).copyImageToBuffer(GetVkImage(*bake.GetCubeImage()),
                                                          vk::ImageLayout::eTransferSrcOptimal,
                                                          GetVkBuffer(*staging), 1, &region);
                cmd.PrepareForAccess(bake.GetCubeView(), AccessKind::SampleGraphics);
            });
        return staging->Download();
    }

    vec4 DecodeTexel(const vector<u8>& faces, const u32 face, const u32 x, const u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(faces.data());
        const usize base = ((static_cast<usize>(face) * FaceSize + y) * FaceSize + x) * 4;
        return {glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                glm::unpackHalf1x16(halves[base + 2]), glm::unpackHalf1x16(halves[base + 3])};
    }

    // The largest per-channel gap, over every texel of the displayed cube, to `expected` of the
    // texel's direction.
    template <class Expected>
    f32 MaxError(const vector<u8>& faces, const Expected& expected)
    {
        f32 worst = 0.0f;
        for (u32 face = 0; face < BakedSkyCube::CubeFaces; ++face)
        {
            for (u32 y = 0; y < FaceSize; ++y)
            {
                for (u32 x = 0; x < FaceSize; ++x)
                {
                    const vec2 uv((static_cast<f32>(x) + 0.5f) / FaceSize,
                                  (static_cast<f32>(y) + 0.5f) / FaceSize);
                    const vec4 gap = glm::abs(DecodeTexel(faces, face, x, y) -
                                              expected(FaceDirection(face, uv)));
                    worst = std::max({worst, gap.x, gap.y, gap.z, gap.w});
                }
            }
        }
        return worst;
    }

    struct FieldFixture
    {
        AssetHandle<MaterialInstance> Field;
        AssetHandle<MaterialInstance> Read;
    };

    FieldFixture CookAndLoad(AssetManager& assets)
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_sky_bake_field.vengpack";
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cooked = Veng::TestSupport::CookCached(
            cooker, fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr, nullptr,
            nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE(cooked.has_value());
        REQUIRE(assets.Mount(outArchive).has_value());

        const AssetResult<AssetHandle<MaterialInstance>> field =
            assets.LoadSync<MaterialInstance>(FieldInstanceId);
        const AssetResult<AssetHandle<MaterialInstance>> read =
            assets.LoadSync<MaterialInstance>(ReadInstanceId);
        REQUIRE(field.has_value());
        REQUIRE(read.has_value());
        return {.Field = *field, .Read = *read};
    }

    // The field layer and one composite layer reading it.
    std::array<BakedSkyCube::SkyBakeLayer, 2>
    FieldLayers(const FieldFixture& fixture, const u32 fieldCost = 1, const u32 pictureCost = 1)
    {
        return {{
            {.Material = fixture.Field.Get(),
             .Role = BakedSkyCube::SkyBakeRole::Field,
             .FieldTargets = 3,
             .FaceSizeDivisor = FieldDivisor,
             .TilePixels = FieldTilePixels,
             .Cost = fieldCost},
            {.Material = fixture.Read.Get(), .Blend = BlendState::Opaque(), .Cost = pictureCost},
        }};
    }

    // Pumps the requested bake to completion and lands it in the displayed cube.
    void FinishBake(Context& context, BakedSkyCube& bake)
    {
        for (u32 frame = 0; frame < 256 && bake.IsBakePending(); ++frame)
        {
            context.BeginFrame();
            context.EndFrame();
        }
        REQUIRE_FALSE(bake.IsBakePending());
        context.ImmediateCommands([&](CommandBuffer& cmd) { CHECK(bake.RecordAmortized(cmd)); });
    }

    // Sets the composite's read: a target, a mode (0 bicubic, 1 mean, 2 mip) and a mip.
    void SetRead(const FieldFixture& fixture, const u32 target, const u32 mode,
                 const f32 lod = 0.0f, const vec4 scale = vec4(1.0f))
    {
        MaterialInstance* read = fixture.Read.Get();
        read->SetParam("Mode", vec4(static_cast<f32>(target), static_cast<f32>(mode), lod, 0.0f));
        read->SetParam("Scale", scale);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake field: the later layers read every field target back, and a "
                  "re-composite over the standing field runs no field tiles")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const FieldFixture fixture = CookAndLoad(assets);
    const Unique<BakedSkyCube> bake =
        BakedSkyCube::Create(Context, BakedSkyCube::CreateConsumerSetLayout(Context),
                             Format::RGBA16Sfloat, FaceSize, &assets);
    GeneratedTextureService& service = Context.GetGeneratedTextures();
    service.SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);
    const auto layers = FieldLayers(fixture);

    // The first bake runs the field: every tile of it, then the composite reading target 0.
    SetRead(fixture, 0, 0);
    bake->RequestBake(service, layers, {}, "field-a");
    FinishBake(Context, *bake);
    CHECK(bake->GetFieldTilesRecorded() == FieldTicks);
    CHECK(bake->IsFieldStanding());
    CHECK(bake->GetFieldFaceSize() == FieldFaceSize);

    // The B-spline smooths by about a sixth of a field texel's width squared (0.125 in face
    // coordinates) times the function's Laplacian on the face; these functions' stay below about
    // ten, so the bound is a few hundredths at every texel, face edges and cube corners included.
    constexpr f32 BicubicTolerance = 0.03f;
    CHECK(MaxError(DownloadCube(Context, *bake), [](const vec3 d) { return FieldFunction(0, d); }) <
          BicubicTolerance);

    // Each later bake names the standing field, so it re-composites with a changed read and no
    // field tile runs.
    for (const u32 target : {1u, 2u})
    {
        SetRead(fixture, target, 0);
        bake->RequestBake(service, layers, {}, "field-a");
        FinishBake(Context, *bake);
        CHECK(MaxError(DownloadCube(Context, *bake),
                       [&](const vec3 d) { return FieldFunction(target, d); }) < BicubicTolerance);
    }
    CHECK(bake->GetFieldTilesRecorded() == FieldTicks);

    // Mip 0 read bilinear interpolates the same texels, within the same bound.
    SetRead(fixture, 0, 2, 0.0f);
    bake->RequestBake(service, layers, {}, "field-a");
    FinishBake(Context, *bake);
    CHECK(MaxError(DownloadCube(Context, *bake), [](const vec3 d) { return FieldFunction(0, d); }) <
          BicubicTolerance);

    // The solid-angle mean of the quartic target: within half-float rounding of the exact texel
    // sum, where a texel-count mean is about 0.02 low.
    SetRead(fixture, 2, 1);
    bake->RequestBake(service, layers, {}, "field-a");
    FinishBake(Context, *bake);
    const vec4 mean = SolidAngleMean(2);
    CHECK(MaxError(DownloadCube(Context, *bake), [&](const vec3) { return mean; }) < 0.003f);
    CHECK(bake->GetFieldTilesRecorded() == FieldTicks);

    // A changed composite (its scale) lands too, over the same standing field.
    SetRead(fixture, 0, 0, 0.0f, vec4(2.0f));
    bake->RequestBake(service, layers, {}, "field-a");
    FinishBake(Context, *bake);
    CHECK(MaxError(DownloadCube(Context, *bake), [](const vec3 d)
                   { return 2.0f * FieldFunction(0, d); }) < 2.0f * BicubicTolerance);
    CHECK(bake->GetFieldTilesRecorded() == FieldTicks);

    service.ClearCostBudget();
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake field: a supersede during the field phase leaves no field standing")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const FieldFixture fixture = CookAndLoad(assets);
    const Unique<BakedSkyCube> bake =
        BakedSkyCube::Create(Context, BakedSkyCube::CreateConsumerSetLayout(Context),
                             Format::RGBA16Sfloat, FaceSize, &assets);
    GeneratedTextureService& service = Context.GetGeneratedTextures();
    service.SetCostBudget(1);
    const auto layers = FieldLayers(fixture);
    SetRead(fixture, 0, 0);

    // A field stands under one key; a bake under another is superseded three tiles in, having
    // overwritten part of the field cubes.
    bake->RequestBake(service, layers, {}, "field-b");
    FinishBake(Context, *bake);
    REQUIRE(bake->IsFieldStanding());
    bake->RequestBake(service, layers, {}, "field-c");
    for (u32 frame = 0; frame < 3; ++frame)
    {
        Context.BeginFrame();
        Context.EndFrame();
    }
    CHECK(bake->GetFieldTilesRecorded() == FieldTicks + 3);
    CHECK_FALSE(bake->IsFieldStanding());

    // Either key again finds nothing standing under it, so the whole field runs.
    bake->RequestBake(service, layers, {}, "field-b");
    FinishBake(Context, *bake);
    CHECK(bake->GetFieldTilesRecorded() == 2 * FieldTicks + 3);
    CHECK(bake->IsFieldStanding());

    service.ClearCostBudget();
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "sky bake field: the field's ticks and the picture's ticks charge their own costs")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const FieldFixture fixture = CookAndLoad(assets);
    const Unique<BakedSkyCube> bake =
        BakedSkyCube::Create(Context, BakedSkyCube::CreateConsumerSetLayout(Context),
                             Format::RGBA16Sfloat, FaceSize, &assets);
    GeneratedTextureService& service = Context.GetGeneratedTextures();

    // A budget of four: a field tile costing four runs alone each pump; the finishing step and the
    // composite's six one-tile faces, at one each, take two pumps.
    service.SetCostBudget(4);
    const auto layers = FieldLayers(fixture, 4, 1);
    SetRead(fixture, 0, 0);
    bake->RequestBake(service, layers);

    u32 fieldPumps = 0;
    u32 lonePumps = 0;
    while (bake->GetFieldTilesRecorded() < FieldTicks && fieldPumps < 64)
    {
        const u64 before = bake->GetFaceRendersRecorded();
        Context.BeginFrame();
        Context.EndFrame();
        lonePumps += bake->GetFaceRendersRecorded() - before == 1 ? 1u : 0u;
        ++fieldPumps;
    }
    CHECK(fieldPumps == FieldTicks);
    CHECK(lonePumps == FieldTicks);

    u32 picturePumps = 0;
    while (bake->IsBakePending() && picturePumps < 64)
    {
        Context.BeginFrame();
        Context.EndFrame();
        ++picturePumps;
    }
    CHECK(picturePumps == 2);

    service.ClearCostBudget();
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake field: a layer reading the field outside a field bake reads zero")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const FieldFixture fixture = CookAndLoad(assets);
    const Unique<BakedSkyCube> bake =
        BakedSkyCube::Create(Context, BakedSkyCube::CreateConsumerSetLayout(Context),
                             Format::RGBA16Sfloat, FaceSize, &assets);
    GeneratedTextureService& service = Context.GetGeneratedTextures();
    service.SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);

    // Bake a field first, so one stands; a picture-only bake still reads none of it.
    SetRead(fixture, 0, 0);
    bake->RequestBake(service, FieldLayers(fixture), {}, "field-c");
    FinishBake(Context, *bake);
    REQUIRE(bake->IsFieldStanding());

    for (const u32 mode : {0u, 1u, 2u})
    {
        SetRead(fixture, 0, mode);
        const BakedSkyCube::SkyBakeLayer picture{.Material = fixture.Read.Get()};
        bake->RequestBake(service, std::span(&picture, 1));
        FinishBake(Context, *bake);
        CHECK(MaxError(DownloadCube(Context, *bake), [](const vec3) { return vec4(0.0f); }) ==
              0.0f);
    }

    service.ClearCostBudget();
}

#endif
