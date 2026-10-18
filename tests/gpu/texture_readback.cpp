// ReadbackTexels: a level of any sampleable colour image reads back as float RGBA, every layer in
// order, with no transfer usage on the image — the properties a texture export relies on.

#include <array>
#include <cmath>

#include <doctest/doctest.h>
#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/TextureReadback.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    /// @brief The value the six-layer fixture stores at a texel: distinct per layer and position.
    vec4 CubeTexel(const u32 layer, const u32 x, const u32 y)
    {
        return {static_cast<f32>(layer) * 10.0f + static_cast<f32>(x), static_cast<f32>(y) * 0.25f,
                0.001f * static_cast<f32>(layer + 1), 1.0f};
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "texture readback: a float cube reads back every face in layer order")
{
    constexpr u32 size = 4;
    constexpr u32 faces = 6;
    AssetManager assets(Context, Tasks, Types);

    // Sampled and uploaded only — no TransferSrc — as a cooked texture is.
    const Ref<Image> cube =
        Image::Create(Context, {
                                   .Name = "Readback Cube",
                                   .Extent = {size, size, 1},
                                   .Layers = faces,
                                   .Format = Format::RGBA16Sfloat,
                                   .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                               });
    vector<u16> halves(static_cast<usize>(size) * size * faces * 4);
    for (u32 layer = 0; layer < faces; ++layer)
    {
        for (u32 y = 0; y < size; ++y)
        {
            for (u32 x = 0; x < size; ++x)
            {
                const vec4 value = CubeTexel(layer, x, y);
                const usize base = ((static_cast<usize>(layer) * size + y) * size + x) * 4;
                for (u32 c = 0; c < 4; ++c)
                {
                    halves[base + c] = glm::packHalf1x16(value[c]);
                }
            }
        }
    }
    cube->UploadSync(
        std::span(reinterpret_cast<const u8*>(halves.data()), halves.size() * sizeof(u16)));

    const Result<TexelReadback> readback = ReadbackTexels(Context, assets, cube);
    REQUIRE(readback.has_value());
    CHECK(readback->Extent == uvec2(size));
    CHECK(readback->Layers == faces);
    REQUIRE(readback->Texels.size() == static_cast<usize>(size) * size * faces);

    f32 worstError = 0.0f;
    for (u32 layer = 0; layer < faces; ++layer)
    {
        for (u32 y = 0; y < size; ++y)
        {
            for (u32 x = 0; x < size; ++x)
            {
                const vec4 expected = CubeTexel(layer, x, y);
                const vec4 read =
                    readback->Texels[(static_cast<usize>(layer) * size + y) * size + x];
                for (u32 c = 0; c < 4; ++c)
                {
                    worstError = std::max(worstError, std::abs(read[c] - expected[c]) /
                                                          std::max(std::abs(expected[c]), 1e-3f));
                }
            }
        }
    }
    // A half carries about three significant digits.
    CHECK(worstError < 1e-3f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "texture readback: an sRGB texture reads back linear")
{
    AssetManager assets(Context, Tasks, Types);
    const Ref<Image> image =
        Image::Create(Context, {
                                   .Name = "Readback sRGB",
                                   .Extent = {2, 1, 1},
                                   .Format = Format::RGBA8Srgb,
                                   .Usage = ImageUsage::Sampled | ImageUsage::TransferDst,
                               });
    const std::array<u8, 8> bytes{0, 128, 255, 255, 188, 188, 188, 255};
    image->UploadSync(bytes);

    const Result<TexelReadback> readback = ReadbackTexels(Context, assets, image);
    REQUIRE(readback.has_value());
    REQUIRE(readback->Texels.size() == 2);
    CHECK(readback->Texels[0].r == doctest::Approx(0.0f));
    CHECK(readback->Texels[0].g == doctest::Approx(0.2158f).epsilon(0.01));
    CHECK(readback->Texels[0].b == doctest::Approx(1.0f));
    CHECK(readback->Texels[1].r == doctest::Approx(0.5029f).epsilon(0.01));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "texture readback: a depth image is refused")
{
    AssetManager assets(Context, Tasks, Types);
    const Ref<Image> depth =
        Image::Create(Context, {
                                   .Name = "Readback Depth",
                                   .Extent = {2, 2, 1},
                                   .Format = Format::D32Sfloat,
                                   .Usage = ImageUsage::Sampled | ImageUsage::DepthAttachment,
                               });
    CHECK_FALSE(ReadbackTexels(Context, assets, depth).has_value());
}
