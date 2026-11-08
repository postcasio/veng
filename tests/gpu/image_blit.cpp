// Image blits at the backend's own level. A blit samples the source at each destination texel's
// centre mapped into the source region, with a linear filter, so a scaled blit's texels are fixed by
// that rule alone: a black-then-white row of two texels upscaled to four reads 0, 1/4, 3/4 and 1
// (the outer centres clamp to the edge texels), and a white-then-black row reversed by a mirrored
// destination region reads them backwards. An unscaled blit copies.

#include <array>

#include <doctest/doctest.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    Ref<Image> MakeImage(Context& context, const string& name, const uvec2 extent)
    {
        return Image::Create(context,
                             {.Name = name,
                              .Extent = {extent.x, extent.y, 1},
                              .Format = Format::RGBA8Unorm,
                              .Usage = ImageUsage::ColorAttachment | ImageUsage::TransferSrc |
                                       ImageUsage::TransferDst | ImageUsage::Sampled});
    }

    // Blits the whole of @p source into the whole of @p destination, mirroring along X when asked.
    void Blit(Context& context, const Ref<Image>& source, const Ref<Image>& destination,
              const bool mirrorX)
    {
        const ivec3 sourceEnd(ivec2(source->GetExtent()), 1);
        const ivec3 destinationEnd(ivec2(destination->GetExtent()), 1);
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                cmd.PrepareForAccess(
                    ImageView::Create(context, {.Name = "Source", .Image = source}),
                    AccessKind::TransferSrc);
                cmd.PrepareForAccess(
                    ImageView::Create(context, {.Name = "Destination", .Image = destination}),
                    AccessKind::TransferDst);
                cmd.BlitImage({
                    .SourceImage = source,
                    .DestinationImage = destination,
                    .SourceMipLevel = 0,
                    .DestinationMipLevel = 0,
                    .SourceOffset = {0, 0, 0},
                    .DestinationOffset = {mirrorX ? destinationEnd.x : 0, 0, 0},
                    .SourceExtent = sourceEnd,
                    .DestinationExtent = {mirrorX ? 0 : destinationEnd.x, destinationEnd.y, 1},
                });
            });
    }

    // The red channel of each texel of a one-row RGBA8 image.
    vector<u8> RedRow(const vector<u8>& pixels)
    {
        vector<u8> red;
        for (usize i = 0; i < pixels.size(); i += 4)
        {
            red.push_back(pixels[i]);
        }
        return red;
    }

    void CheckNear(const vector<u8>& actual, const std::array<u8, 4>& expected)
    {
        REQUIRE(actual.size() == expected.size());
        for (usize i = 0; i < expected.size(); ++i)
        {
            CHECK(std::abs(static_cast<int>(actual[i]) - static_cast<int>(expected[i])) <= 2);
        }
    }

    const std::array<u8, 8> BlackWhite{0, 0, 0, 255, 255, 255, 255, 255};
    const std::array<u8, 4> Ramp{0, 64, 191, 255};
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "image blit: an upscaled row reads the texels the linear filter determines")
{
    const Ref<Image> source = MakeImage(Context, "Blit Source", {2, 1});
    source->UploadSync(std::span<const u8>(BlackWhite));
    const Ref<Image> destination = MakeImage(Context, "Blit Destination", {4, 1});

    Blit(Context, source, destination, false);
    CheckNear(RedRow(destination->Download()), Ramp);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "image blit: a mirrored destination region reverses the filtered row")
{
    const Ref<Image> source = MakeImage(Context, "Blit Source", {2, 1});
    source->UploadSync(std::span<const u8>(BlackWhite));
    const Ref<Image> destination = MakeImage(Context, "Blit Destination", {4, 1});

    Blit(Context, source, destination, true);
    CheckNear(RedRow(destination->Download()), {Ramp[3], Ramp[2], Ramp[1], Ramp[0]});
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "image blit: an unscaled blit copies every texel")
{
    const Ref<Image> source = MakeImage(Context, "Blit Source", {2, 1});
    source->UploadSync(std::span<const u8>(BlackWhite));
    const Ref<Image> destination = MakeImage(Context, "Blit Destination", {2, 1});

    Blit(Context, source, destination, false);
    CHECK(destination->Download() == vector<u8>(BlackWhite.begin(), BlackWhite.end()));
}
