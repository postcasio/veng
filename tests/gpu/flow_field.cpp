// FlowField against a live device: the two properties the shaders must keep, each seeded on the
// CPU, stepped through the real compute kernels, read back through the frame-deferred readback
// primitive, and asserted numerically.
//
//   1. Advection transports — a dye on a known constant field lands at the offset the device-free
//      back-trace predicts, undiffused for a whole-texel step, so the shader and the CPU reference
//      agree.
//   2. Sharpen holds detail and stays bounded — a sharpened feedback advection carries more local
//      contrast than the un-sharpened control, and its dye never climbs above the seed's range,
//      the clamp doing its job on the real GPU path over a long loop.
//   3. A velocity rewritten mid-run is read live — a dye advected under field A, then under a field
//      B the test writes into the velocity image between advects (in one command buffer), lands
//      where B's oracle predicts and not A's, so the advect reads the field the moment it records.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <vector>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/AsyncReadback.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/FlowField.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr ImageUsage FieldUsage = ImageUsage::Sampled | ImageUsage::Storage |
                                      ImageUsage::TransferSrc | ImageUsage::TransferDst;

    Ref<Image> MakeField(Context& context, const char* name, const uvec2 extent,
                         const Format format)
    {
        return Image::Create(context, {
                                          .Name = name,
                                          .Extent = {extent.x, extent.y, 1},
                                          .Format = format,
                                          .Usage = FieldUsage,
                                      });
    }

    template <typename Fn>
    void SeedVelocity(const Ref<Image>& image, const uvec2 extent, Fn value)
    {
        const usize texels = static_cast<usize>(extent.x) * extent.y;
        std::vector<f32> data(texels * 2);
        for (u32 y = 0; y < extent.y; ++y)
        {
            for (u32 x = 0; x < extent.x; ++x)
            {
                const vec2 v = value(x, y);
                const usize base = (static_cast<usize>(y) * extent.x + x) * 2;
                data[base + 0] = v.x;
                data[base + 1] = v.y;
            }
        }
        image->UploadSync(
            std::span(reinterpret_cast<const u8*>(data.data()), data.size() * sizeof(f32)));
    }

    template <typename Fn>
    void SeedDye(const Ref<Image>& image, const uvec2 extent, Fn value)
    {
        std::vector<u16> data(static_cast<usize>(extent.x) * extent.y);
        for (u32 y = 0; y < extent.y; ++y)
        {
            for (u32 x = 0; x < extent.x; ++x)
            {
                data[static_cast<usize>(y) * extent.x + x] =
                    static_cast<u16>(glm::packHalf1x16(value(x, y)));
            }
        }
        image->UploadSync(
            std::span(reinterpret_cast<const u8*>(data.data()), data.size() * sizeof(u16)));
    }

    // Reads a whole image back through the frame-deferred primitive, driving frames until the bytes
    // are delivered. Never blocks a fence by hand — the readback has no wait path.
    std::vector<u8> ReadField(Context& context, const Ref<Image>& image)
    {
        AsyncReadback& readback = context.GetAsyncReadback();
        std::vector<u8> bytes;
        bool delivered = false;
        const AsyncReadbackHandle handle = readback.Request({
            .Name = "FlowFieldRead",
            .Image = image,
            .OnComplete =
                [&bytes, &delivered](const std::span<const u8> data)
            {
                bytes.assign(data.begin(), data.end());
                delivered = true;
            },
        });
        REQUIRE(handle.IsValid());

        u32 frames = 0;
        while (!delivered && frames < 32)
        {
            context.BeginFrame();
            context.EndFrame();
            frames++;
        }
        REQUIRE(delivered);
        return bytes;
    }

    // An R16Sfloat dye read back as a scalar accessor.
    class DyeReader
    {
    public:
        DyeReader(std::vector<u8> bytes, const uvec2 extent)
            : m_Bytes(std::move(bytes)), m_Extent(extent)
        {
        }

        [[nodiscard]] f32 At(const u32 x, const u32 y) const
        {
            const auto* data = reinterpret_cast<const u16*>(m_Bytes.data());
            return glm::unpackHalf1x16(data[static_cast<usize>(y) * m_Extent.x + x]);
        }

    private:
        std::vector<u8> m_Bytes;
        uvec2 m_Extent;
    };

    DyeReader ReadDye(Context& context, const Ref<Image>& image, const uvec2 extent)
    {
        return {ReadField(context, image), extent};
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "flow field: a dye advects to the offset the back-trace predicts")
{
    AssetManager assets(Context, Tasks, Types);
    constexpr uvec2 Extent{32, 16};
    constexpr u32 Steps = 12;
    constexpr u32 StartColumn = 4;

    const Ref<Image> velocity = MakeField(Context, "Flow Velocity", Extent, Format::RG32Sfloat);
    const Ref<Image> dye = MakeField(Context, "Flow Dye", Extent, Format::R16Sfloat);

    // A uniform unit flow along +x and a single bright column: whole-texel steps tap exactly one
    // source texel, so the blob is transported undiffused across the periodic seam.
    SeedVelocity(velocity, Extent, [](u32, u32) { return vec2(1.0f, 0.0f); });
    SeedDye(dye, Extent, [](const u32 x, u32) { return x == StartColumn ? 1.0f : 0.0f; });

    const Unique<FlowField> flow = FlowField::Create(Context, assets,
                                                     {
                                                         .Name = "Advect",
                                                         .Velocity = velocity,
                                                         .Dyes = {dye},
                                                         .Shape =
                                                             {
                                                                 .WrapX = FlowWrap::Periodic,
                                                                 .WrapY = FlowWrap::Clamped,
                                                                 .StepScale = 1.0f,
                                                             },
                                                     });
    Context.ImmediateCommands([&](CommandBuffer& cmd) { flow->RecordAdvect(cmd, Steps); });
    CHECK(flow->GetAdvectCount() == Steps);

    const DyeReader field = ReadDye(Context, dye, Extent);
    std::vector<f32> columns(Extent.x, 0.0f);
    for (u32 x = 0; x < Extent.x; ++x)
    {
        for (u32 y = 0; y < Extent.y; ++y)
        {
            columns[x] += field.At(x, y);
        }
    }

    // The oracle: the device-free back-trace moves the front one whole texel per step, so after
    // Steps it sits at (StartColumn + Steps), wrapped. The peak column is exactly there, and it
    // holds the seeded mass undiffused.
    const u32 expected = (StartColumn + Steps) % Extent.x;
    const auto peak = std::distance(columns.begin(), std::ranges::max_element(columns));
    CHECK(static_cast<u32>(peak) == expected);
    CHECK(columns[expected] == doctest::Approx(static_cast<f32>(Extent.y)).epsilon(0.02));
    for (u32 x = 0; x < Extent.x; ++x)
    {
        if (x != expected)
        {
            CHECK(columns[x] == doctest::Approx(0.0f).epsilon(0.02));
        }
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "flow field: sharpen raises local contrast against an un-sharpened control")
{
    AssetManager assets(Context, Tasks, Types);
    constexpr uvec2 Extent{32, 32};
    constexpr u32 BlurSteps = 6;
    constexpr f32 Strength = 2.0f;
    constexpr f32 SeedMax = 0.85f;
    constexpr f32 SeedMin = 0.15f;

    // A smooth sinusoid carried by a sub-texel flow: the fractional advection low-passes it every
    // step, bleeding its high-frequency content away — exactly what a feedback advection does to any
    // dye, and what the sharpen exists to hold.
    auto sinusoid = [](const u32 x, u32)
    { return 0.5f + 0.35f * std::sin(6.2831853f * static_cast<f32>(x) / 8.0f); };

    // Acutance: the dye's mean deviation from its own 3x3 blur — the high-frequency energy a
    // low-pass advection destroys and a sharpen restores. It is exactly what the clamped unsharp
    // raises (the gradient of a monotone ramp is merely redistributed by an anti-ringing clamp, so
    // it is the wrong thing to measure; acutance is the right one).
    auto Acutance = [&](const DyeReader& field)
    {
        f32 sum = 0.0f;
        u32 count = 0;
        for (u32 y = 1; y + 1 < Extent.y; ++y)
        {
            for (u32 x = 1; x + 1 < Extent.x; ++x)
            {
                f32 blur = 0.0f;
                for (i32 dy = -1; dy <= 1; ++dy)
                {
                    for (i32 dx = -1; dx <= 1; ++dx)
                    {
                        blur += field.At(x + dx, y + dy);
                    }
                }
                sum += std::abs(field.At(x, y) - blur / 9.0f);
                count++;
            }
        }
        return sum / static_cast<f32>(count);
    };

    auto Run = [&](auto&& record)
    {
        const Ref<Image> velocity =
            MakeField(Context, "Sharpen Velocity", Extent, Format::RG32Sfloat);
        const Ref<Image> dye = MakeField(Context, "Sharpen Dye", Extent, Format::R16Sfloat);
        SeedVelocity(velocity, Extent, [](u32, u32) { return vec2(0.4f, 0.0f); });
        SeedDye(dye, Extent, sinusoid);

        const Unique<FlowField> flow = FlowField::Create(Context, assets,
                                                         {
                                                             .Name = "Sharpen",
                                                             .Velocity = velocity,
                                                             .Dyes = {dye},
                                                             .Shape =
                                                                 {
                                                                     .WrapX = FlowWrap::Periodic,
                                                                     .WrapY = FlowWrap::Periodic,
                                                                     .StepScale = 1.0f,
                                                                 },
                                                         });
        Context.ImmediateCommands([&](CommandBuffer& cmd) { record(*flow, cmd); });
        return ReadDye(Context, dye, Extent);
    };

    // The control and the test share the same blurred prefix (the advection is deterministic), so
    // the only difference is the sharpen the test applies after — isolating what it does.
    const f32 control = Acutance(
        Run([&](FlowField& flow, CommandBuffer& cmd) { flow.RecordAdvect(cmd, BlurSteps); }));
    const f32 sharpened = Acutance(Run(
        [&](FlowField& flow, CommandBuffer& cmd)
        {
            flow.RecordAdvect(cmd, BlurSteps);
            flow.RecordSharpen(cmd, Strength);
            flow.RecordSharpen(cmd, Strength);
        }));

    // Sharpening the blurred field restores meaningfully more high-frequency energy than the
    // advection left behind.
    CHECK(sharpened > control * 1.2f);

    // Over a long feedback loop the clamp keeps the dye inside the seed's own range — no texel climbs
    // above the brightest seed value nor sinks below the darkest, so the loop cannot diverge.
    const DyeReader longRun = Run(
        [&](FlowField& flow, CommandBuffer& cmd)
        {
            for (u32 step = 0; step < 40; ++step)
            {
                flow.RecordAdvect(cmd);
                flow.RecordSharpen(cmd, Strength);
            }
        });
    f32 runMax = 0.0f;
    f32 runMin = std::numeric_limits<f32>::max();
    for (u32 y = 0; y < Extent.y; ++y)
    {
        for (u32 x = 0; x < Extent.x; ++x)
        {
            runMax = std::max(runMax, longRun.At(x, y));
            runMin = std::min(runMin, longRun.At(x, y));
        }
    }
    CHECK(runMax <= SeedMax + 0.02f);
    CHECK(runMin >= SeedMin - 0.02f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "flow field: a velocity rewritten mid-run advects the dye along the new field")
{
    AssetManager assets(Context, Tasks, Types);
    constexpr uvec2 Extent{32, 32};
    constexpr u32 StepsA = 5;
    constexpr u32 StepsB = 7;
    constexpr u32 StartColumn = 3;
    constexpr u32 StartRow = 4;

    const Ref<Image> velocity = MakeField(Context, "Evolving Velocity", Extent, Format::RG32Sfloat);
    const Ref<Image> dye = MakeField(Context, "Evolving Dye", Extent, Format::R16Sfloat);

    // Field A flows +x, field B flows +y. A single bright texel tracks each segment's whole-texel
    // steps undiffused, so its final cell is a clean oracle for which field the second segment read:
    // B moves it in y, an axis A (zero y-velocity) can never touch.
    SeedVelocity(velocity, Extent, [](u32, u32) { return vec2(1.0f, 0.0f); });
    SeedDye(dye, Extent, [](const u32 x, const u32 y)
            { return (x == StartColumn && y == StartRow) ? 1.0f : 0.0f; });

    // Field B staged for a mid-command-buffer transfer into the velocity image.
    std::vector<f32> fieldB(static_cast<usize>(Extent.x) * Extent.y * 2);
    for (usize texel = 0; texel < static_cast<usize>(Extent.x) * Extent.y; ++texel)
    {
        fieldB[texel * 2 + 0] = 0.0f;
        fieldB[texel * 2 + 1] = 1.0f;
    }
    const Ref<Buffer> stagingB = Buffer::Create(Context, {
                                                             .Name = "Field B Staging",
                                                             .Size = fieldB.size() * sizeof(f32),
                                                             .Usage = BufferUsage::TransferSrc,
                                                         });
    stagingB->UploadSync(
        std::span(reinterpret_cast<const u8*>(fieldB.data()), fieldB.size() * sizeof(f32)));

    // A caller-owned view of the velocity: layout tracking is per-image, so transitioning through
    // this view is exactly what FlowField's own advect barrier sees.
    const Ref<ImageView> velocityView =
        ImageView::Create(Context, {.Name = "Evolving Velocity View", .Image = velocity});

    const Unique<FlowField> flow = FlowField::Create(Context, assets,
                                                     {
                                                         .Name = "Evolving",
                                                         .Velocity = velocity,
                                                         .Dyes = {dye},
                                                         .Shape =
                                                             {
                                                                 .WrapX = FlowWrap::Periodic,
                                                                 .WrapY = FlowWrap::Periodic,
                                                                 .StepScale = 1.0f,
                                                             },
                                                     });

    // Advect along A, then rewrite the velocity to B in the sanctioned dance — transition it to a
    // transfer destination, copy B in, and let the next advect's own sample barrier make the write
    // visible — then advect along B, all in one command buffer.
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            flow->RecordAdvect(cmd, StepsA);
            cmd.PrepareForAccess(velocityView, AccessKind::TransferDst);
            cmd.CopyBufferToImage(stagingB, velocity);
            flow->RecordAdvect(cmd, StepsB);
        });
    CHECK(flow->GetAdvectCount() == StepsA + StepsB);

    const DyeReader field = ReadDye(Context, dye, Extent);

    // The oracle: A carried the texel +x by StepsA, then B carried it +y by StepsB. Reading B is the
    // only way the row moves — had the advect kept reading A, the texel would sit at
    // (StartColumn + StepsA + StepsB, StartRow): the same row, a further column.
    const u32 expectedColumn = (StartColumn + StepsA) % Extent.x;
    const u32 expectedRow = (StartRow + StepsB) % Extent.y;

    uvec2 peak{0, 0};
    f32 peakValue = -1.0f;
    for (u32 y = 0; y < Extent.y; ++y)
    {
        for (u32 x = 0; x < Extent.x; ++x)
        {
            if (const f32 value = field.At(x, y); value > peakValue)
            {
                peakValue = value;
                peak = {x, y};
            }
        }
    }
    CHECK(peak.x == expectedColumn);
    CHECK(peak.y == expectedRow);
    CHECK(field.At(expectedColumn, expectedRow) == doctest::Approx(1.0f).epsilon(0.02));
}
