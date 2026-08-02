// FluidSim against a live device: the stable-fluids solver's six properties, each seeded on the
// CPU, stepped through the real compute kernels, read back through the frame-deferred readback
// primitive, and asserted numerically.
//
//   1. Projection projects — a seeded divergent field comes back divergence-free everywhere but
//      the clamped rows' outermost texels, and one advected dye texel matches the CPU reference
//      tap exactly.
//   2. A vortex persists and transports — a Gaussian vortex in a uniform flow reappears at the
//      advected position, and confinement holds more of its peak vorticity than no confinement.
//   3. Periodic means periodic — a dye blob crosses the seam intact on a periodic x axis and is
//      stopped short of it by the wall on a clamped one.
//   4. The metric bends the path — rows of metric 0.5 advance a dye front half as far.
//   5. Relaxation holds a jet — a zero field converges on its target at exactly the configured
//      rate, and rate 0 leaves it alone.
//   6. Determinism — a fixed seed, configuration and step count hash to a pinned digest.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/AsyncReadback.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/FluidSim.h>
#include <Veng/Renderer/Image.h>
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

    // Seeds a two-channel velocity field from a per-texel function, in whichever of the two
    // velocity formats the image carries.
    template <typename Fn>
    void SeedVelocity(const Ref<Image>& image, const uvec2 extent, Fn value)
    {
        const usize texels = static_cast<usize>(extent.x) * extent.y;
        if (image->GetFormat() == Format::RG32Sfloat)
        {
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
            return;
        }

        std::vector<u16> data(texels * 2);
        for (u32 y = 0; y < extent.y; ++y)
        {
            for (u32 x = 0; x < extent.x; ++x)
            {
                const vec2 v = value(x, y);
                const usize base = (static_cast<usize>(y) * extent.x + x) * 2;
                data[base + 0] = static_cast<u16>(glm::packHalf1x16(v.x));
                data[base + 1] = static_cast<u16>(glm::packHalf1x16(v.y));
            }
        }
        image->UploadSync(
            std::span(reinterpret_cast<const u8*>(data.data()), data.size() * sizeof(u16)));
    }

    // Seeds a single-channel R16Sfloat dye field.
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

    // Reads a whole image back through the frame-deferred primitive, driving frames until the
    // bytes are delivered. Never blocks a fence by hand — the readback has no wait path.
    std::vector<u8> ReadField(Context& context, const Ref<Image>& image)
    {
        AsyncReadback& readback = context.GetAsyncReadback();
        std::vector<u8> bytes;
        bool delivered = false;
        const AsyncReadbackHandle handle = readback.Request({
            .Name = "FluidFieldRead",
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

    // A read-back field as a texel accessor, decoding whichever format it was written in.
    class FieldReader
    {
    public:
        FieldReader(std::vector<u8> bytes, const uvec2 extent, const Format format)
            : m_Bytes(std::move(bytes)), m_Extent(extent), m_Format(format)
        {
        }

        [[nodiscard]] vec2 Velocity(const u32 x, const u32 y) const
        {
            const usize index = (static_cast<usize>(y) * m_Extent.x + x) * 2;
            if (m_Format == Format::RG32Sfloat)
            {
                const auto* data = reinterpret_cast<const f32*>(m_Bytes.data());
                return {data[index], data[index + 1]};
            }
            const auto* data = reinterpret_cast<const u16*>(m_Bytes.data());
            return {glm::unpackHalf1x16(data[index]), glm::unpackHalf1x16(data[index + 1])};
        }

        [[nodiscard]] f32 Scalar(const u32 x, const u32 y) const
        {
            const auto* data = reinterpret_cast<const u16*>(m_Bytes.data());
            return glm::unpackHalf1x16(data[static_cast<usize>(y) * m_Extent.x + x]);
        }

    private:
        std::vector<u8> m_Bytes;
        uvec2 m_Extent;
        Format m_Format;
    };

    FieldReader Read(Context& context, const Ref<Image>& image, const uvec2 extent)
    {
        return {ReadField(context, image), extent, image->GetFormat()};
    }

    // The same divergence stencil fluid_divergence.comp uses, on the CPU, so the projection is
    // measured against its own discretization rather than an analytic one.
    f32 Divergence(const FieldReader& field, const uvec2 extent, const u32 x, const u32 y,
                   const FluidWrap wrapX, const FluidWrap wrapY)
    {
        auto Sample = [&](const i32 dx, const i32 dy)
        {
            return field.Velocity(
                static_cast<u32>(FoldFluidTexel(static_cast<i32>(x) + dx, extent.x, wrapX)),
                static_cast<u32>(FoldFluidTexel(static_cast<i32>(y) + dy, extent.y, wrapY)));
        };
        return (Sample(1, 0).x - Sample(-1, 0).x) * 0.5f +
               (Sample(0, 1).y - Sample(0, -1).y) * 0.5f;
    }

    f32 Curl(const FieldReader& field, const uvec2 extent, const u32 x, const u32 y)
    {
        auto Sample = [&](const i32 dx, const i32 dy)
        {
            return field.Velocity(static_cast<u32>(FoldFluidTexel(static_cast<i32>(x) + dx,
                                                                  extent.x, FluidWrap::Periodic)),
                                  static_cast<u32>(FoldFluidTexel(static_cast<i32>(y) + dy,
                                                                  extent.y, FluidWrap::Periodic)));
        };
        return (Sample(1, 0).y - Sample(-1, 0).y) * 0.5f -
               (Sample(0, 1).x - Sample(0, -1).x) * 0.5f;
    }

    // FNV-1a over a byte span — the determinism digest's hash.
    u64 Digest(const std::vector<u8>& bytes)
    {
        u64 hash = 0xCBF2'9CE4'8422'2325ull;
        for (const u8 byte : bytes)
        {
            hash ^= byte;
            hash *= 0x0000'0100'0000'01B3ull;
        }
        return hash;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "fluid sim: the projection leaves the interior divergence-free")
{
    AssetManager assets(Context, Tasks, Types);
    constexpr uvec2 Extent{32, 32};
    constexpr FluidWrap WrapX = FluidWrap::Periodic;
    constexpr FluidWrap WrapY = FluidWrap::Clamped;
    constexpr f32 TimeStep = 0.1f;
    // The wall texels are excluded from the divergence sweep, and so is the row beside each:
    // the gradient pass zeroes the wall-normal component after the solve, and the centred
    // stencil is two texels wide, so that edit reaches one row in.
    constexpr u32 WallMargin = 2;

    const Ref<Image> velocity = MakeField(Context, "Fluid Velocity", Extent, Format::RG32Sfloat);
    const Ref<Image> dye = MakeField(Context, "Fluid Dye", Extent, Format::R16Sfloat);

    // A deliberately compressible seed: both components vary along their own axis, so the
    // discrete divergence is large everywhere before the solve runs.
    auto seed = [](const u32 x, const u32 y)
    {
        const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(Extent.x);
        const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(Extent.y);
        return vec2(0.8f * std::sin(6.2831853f * u), 0.6f * std::sin(6.2831853f * v));
    };
    SeedVelocity(velocity, Extent, seed);
    SeedDye(dye, Extent,
            [](const u32 x, const u32 y) { return static_cast<f32>((x * 7 + y * 3) % 11) * 0.1f; });

    const FieldReader seeded = Read(Context, velocity, Extent);
    const FieldReader seededDye = Read(Context, dye, Extent);

    f32 before = 0.0f;
    for (u32 y = WallMargin; y + WallMargin < Extent.y; ++y)
    {
        for (u32 x = 0; x < Extent.x; ++x)
        {
            before = std::max(before, std::abs(Divergence(seeded, Extent, x, y, WrapX, WrapY)));
        }
    }
    CHECK(before > 0.05f);

    const Unique<FluidSim> sim = FluidSim::Create(Context, assets,
                                                  {
                                                      .Name = "Projection",
                                                      .Velocity = velocity,
                                                      .Dyes = {{.Field = dye}},
                                                      .WrapX = WrapX,
                                                      .WrapY = WrapY,
                                                      .TimeStep = TimeStep,
                                                      .JacobiIterations = 2000,
                                                  });
    Context.ImmediateCommands([&](CommandBuffer& cmd) { sim->RecordStep(cmd); });
    CHECK(sim->GetStepCount() == 1u);

    const FieldReader projected = Read(Context, velocity, Extent);
    f32 after = 0.0f;
    for (u32 y = WallMargin; y + WallMargin < Extent.y; ++y)
    {
        for (u32 x = 0; x < Extent.x; ++x)
        {
            after = std::max(after, std::abs(Divergence(projected, Extent, x, y, WrapX, WrapY)));
        }
    }
    // What is left is the collocated grid's own floor, not a convergence failure: divergence and
    // the pressure gradient are two-texel-wide differences while the Jacobi stencil is the
    // compact Laplacian, so the projection removes all but a sin^2(k/2) share of each mode —
    // ~2.4% here, measured against the seeded field's own divergence. It is inherent to the
    // scheme (a staggered grid is what removes it) and stays flat however many iterations run.
    CHECK(after < 0.01f);
    CHECK(after < before * 0.05f);

    // One advected dye texel against the CPU reference tap. The dye rode the projected velocity
    // that was just read back, so the expected value is the reference advection of the seeded
    // dye at exactly that velocity — the whole advection kernel, cross-checked.
    const FieldReader advected = Read(Context, dye, Extent);
    constexpr uvec2 Probe{13, 17};
    const vec2 source = FluidBackTrace(Probe, projected.Velocity(Probe.x, Probe.y), TimeStep, 1.0f);
    const vec4 expected = SampleFluidBilinear(
        source, Extent, WrapX, WrapY,
        [&](const ivec2 texel)
        {
            return vec4(seededDye.Scalar(static_cast<u32>(texel.x), static_cast<u32>(texel.y)),
                        0.0f, 0.0f, 0.0f);
        });
    CHECK(advected.Scalar(Probe.x, Probe.y) == doctest::Approx(expected.x).epsilon(0.005));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "fluid sim: a vortex transports with the flow and confinement holds its peak")
{
    AssetManager assets(Context, Tasks, Types);
    constexpr uvec2 Extent{64, 64};
    constexpr u32 Steps = 16;
    constexpr f32 Flow = 0.5f;
    constexpr f32 CentreX = 16.0f;
    constexpr f32 CentreY = 32.0f;
    constexpr f32 Sigma = 5.0f;

    // A Gaussian vortex riding a uniform flow. Both are divergence-free, so the projection has
    // nothing to correct and what the case measures is advection plus confinement alone.
    auto seed = [](const u32 x, const u32 y)
    {
        const f32 dx = static_cast<f32>(x) + 0.5f - CentreX;
        const f32 dy = static_cast<f32>(y) + 0.5f - CentreY;
        const f32 falloff = std::exp(-(dx * dx + dy * dy) / (2.0f * Sigma * Sigma));
        return vec2(Flow, 0.0f) + 0.25f * falloff * vec2(-dy, dx);
    };

    auto Run = [&](const f32 confinement)
    {
        const Ref<Image> velocity =
            MakeField(Context, "Vortex Velocity", Extent, Format::RG32Sfloat);
        SeedVelocity(velocity, Extent, seed);
        const Unique<FluidSim> sim = FluidSim::Create(Context, assets,
                                                      {
                                                          .Name = "Vortex",
                                                          .Velocity = velocity,
                                                          .WrapX = FluidWrap::Periodic,
                                                          .WrapY = FluidWrap::Periodic,
                                                          .TimeStep = 1.0f,
                                                          .JacobiIterations = 40,
                                                          .VorticityConfinement = confinement,
                                                      });
        Context.ImmediateCommands([&](CommandBuffer& cmd) { sim->RecordSteps(cmd, Steps); });
        return Read(Context, velocity, Extent);
    };

    // The vortex's peak |curl| and its vorticity centroid. The centroid is what locates it: a
    // diffused core has a nearly flat top, so the single strongest texel jitters by a texel or
    // two while the weighted centre does not. x is averaged circularly, since the axis is a ring
    // the vortex crosses no part of only by accident.
    auto Locate = [&](const FieldReader& field)
    {
        f32 peak = 0.0f;
        for (u32 y = 0; y < Extent.y; ++y)
        {
            for (u32 x = 0; x < Extent.x; ++x)
            {
                peak = std::max(peak, std::abs(Curl(field, Extent, x, y)));
            }
        }

        constexpr f32 Tau = 6.2831853f;
        f32 weight = 0.0f;
        f32 sumSin = 0.0f;
        f32 sumCos = 0.0f;
        f32 sumY = 0.0f;
        for (u32 y = 0; y < Extent.y; ++y)
        {
            for (u32 x = 0; x < Extent.x; ++x)
            {
                const f32 magnitude = std::abs(Curl(field, Extent, x, y));
                if (magnitude < peak * 0.25f)
                {
                    continue;
                }
                const f32 angle = Tau * (static_cast<f32>(x) + 0.5f) / static_cast<f32>(Extent.x);
                weight += magnitude;
                sumSin += magnitude * std::sin(angle);
                sumCos += magnitude * std::cos(angle);
                sumY += magnitude * (static_cast<f32>(y) + 0.5f);
            }
        }
        REQUIRE(weight > 0.0f);

        f32 angle = std::atan2(sumSin, sumCos);
        if (angle < 0.0f)
        {
            angle += Tau;
        }
        const vec2 centre{angle / Tau * static_cast<f32>(Extent.x), sumY / weight};
        return std::pair{peak, centre};
    };

    const Ref<Image> initial = MakeField(Context, "Vortex Seed", Extent, Format::RG32Sfloat);
    SeedVelocity(initial, Extent, seed);
    const auto [startPeak, startAt] = Locate(Read(Context, initial, Extent));
    CHECK(startPeak > 0.0f);
    CHECK(startAt.x == doctest::Approx(CentreX).epsilon(0.05));

    const auto [freePeak, freeAt] = Locate(Run(0.0f));
    const auto [confinedPeak, confinedAt] = Locate(Run(0.2f));

    // The vortex arrives where the uniform flow carried it, both with and without confinement,
    // and pure transport leaves it on its own row. The confined run's centroid is not held to
    // that: confinement is a force, and an advected core is not quite symmetric for it to push
    // on, so it nudges the core a texel or two off-row over the run.
    const f32 expectedX = CentreX + (Flow * static_cast<f32>(Steps));
    CHECK(freeAt.x == doctest::Approx(expectedX).epsilon(0.05));
    CHECK(confinedAt.x == doctest::Approx(expectedX).epsilon(0.05));
    CHECK(std::abs(freeAt.y - startAt.y) < 1.0f);

    // Semi-Lagrangian advection diffuses — sixteen steps cost this core roughly half its peak
    // vorticity. Confinement is what puts that back, which is the one job it has.
    CHECK(freePeak > startPeak * 0.5f);
    CHECK(confinedPeak > freePeak * 2.0f);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "fluid sim: a periodic axis carries a dye across the seam, a clamped one walls it")
{
    AssetManager assets(Context, Tasks, Types);
    constexpr uvec2 Extent{32, 16};
    constexpr u32 Steps = 40;
    constexpr u32 StartColumn = 4;

    auto ColumnSums = [&](const FluidWrap wrapX)
    {
        // RG16Sfloat here, so the narrow velocity format's force and gradient variants are the
        // ones this case runs.
        const Ref<Image> velocity = MakeField(Context, "Seam Velocity", Extent, Format::RG16Sfloat);
        const Ref<Image> dye = MakeField(Context, "Seam Dye", Extent, Format::R16Sfloat);

        SeedVelocity(velocity, Extent, [](u32, u32) { return vec2(1.0f, 0.0f); });
        SeedDye(dye, Extent, [](const u32 x, u32) { return x == StartColumn ? 1.0f : 0.0f; });

        const Unique<FluidSim> sim = FluidSim::Create(Context, assets,
                                                      {
                                                          .Name = "Seam",
                                                          .Velocity = velocity,
                                                          .Dyes = {{.Field = dye}},
                                                          .WrapX = wrapX,
                                                          .WrapY = FluidWrap::Clamped,
                                                          .TimeStep = 1.0f,
                                                          .JacobiIterations = 30,
                                                      });
        Context.ImmediateCommands([&](CommandBuffer& cmd) { sim->RecordSteps(cmd, Steps); });

        const FieldReader field = Read(Context, dye, Extent);
        std::vector<f32> columns(Extent.x, 0.0f);
        for (u32 x = 0; x < Extent.x; ++x)
        {
            for (u32 y = 0; y < Extent.y; ++y)
            {
                columns[x] += field.Scalar(x, y);
            }
        }
        return columns;
    };

    const std::vector<f32> periodic = ColumnSums(FluidWrap::Periodic);
    const std::vector<f32> clamped = ColumnSums(FluidWrap::Clamped);

    auto Total = [](const std::vector<f32>& columns)
    { return std::accumulate(columns.begin(), columns.end(), 0.0f); };
    auto Centre = [&](const std::vector<f32>& columns)
    {
        f32 weighted = 0.0f;
        for (u32 x = 0; x < columns.size(); ++x)
        {
            weighted += columns[x] * static_cast<f32>(x);
        }
        return weighted / Total(columns);
    };

    // The periodic run conserves the dye exactly: the seeded column carries one unit per row,
    // nothing dissipates, and a uniform flow neither converges nor diverges. The clamped run's
    // total is only bounded below — semi-Lagrangian advection is not conservative where a flow
    // converges, and decelerating against a wall is exactly that, so the dye piling up in front
    // of it gains mass rather than merely stacking.
    const f32 seeded = static_cast<f32>(Extent.y);
    CHECK(Total(periodic) == doctest::Approx(seeded).epsilon(0.01));
    CHECK(Total(clamped) > seeded * 0.5f);

    // Periodic: a uniform unit flow is divergence-free and survives the projection untouched, so
    // forty whole-texel steps put the blob down in one piece at column 12, having crossed the
    // seam once — undiffused, because a whole-texel displacement taps exactly one texel.
    const u32 expected = (StartColumn + Steps) % Extent.x;
    CHECK(periodic[expected] == doctest::Approx(seeded).epsilon(0.01));

    // Clamped: the same sim cannot leave. The wall zeroes the flow at its own texel, pressure
    // builds against it, and the dye halts about a third of the way along with none of it
    // reaching either the wall or the seam the periodic run crossed.
    CHECK(Centre(clamped) > static_cast<f32>(StartColumn));
    CHECK(Centre(clamped) < 20.0f);
    for (u32 x = 0; x <= StartColumn; ++x)
    {
        CHECK(clamped[x] == doctest::Approx(0.0f).epsilon(0.001));
    }
    CHECK(clamped[Extent.x - 1] < 0.01f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "fluid sim: a per-row metric of one half advances a front half as far")
{
    AssetManager assets(Context, Tasks, Types);
    constexpr uvec2 Extent{48, 16};
    constexpr u32 Steps = 10;
    constexpr u32 StartColumn = 4;

    const Ref<Image> velocity = MakeField(Context, "Metric Velocity", Extent, Format::RG32Sfloat);
    const Ref<Image> target = MakeField(Context, "Metric Target", Extent, Format::RG32Sfloat);
    const Ref<Image> dye = MakeField(Context, "Metric Dye", Extent, Format::R16Sfloat);

    auto uniform = [](u32, u32) { return vec2(1.0f, 0.0f); };
    SeedVelocity(velocity, Extent, uniform);
    SeedVelocity(target, Extent, uniform);
    SeedDye(dye, Extent, [](const u32 x, u32) { return x == StartColumn ? 1.0f : 0.0f; });

    // The top half of the grid is stretched: those rows advance half a cell per unit of x
    // velocity, the bottom half a whole one.
    vector<f32> metric(Extent.y, 1.0f);
    for (u32 y = Extent.y / 2; y < Extent.y; ++y)
    {
        metric[y] = 0.5f;
    }

    const Unique<FluidSim> sim = FluidSim::Create(Context, assets,
                                                  {
                                                      .Name = "Metric",
                                                      .Velocity = velocity,
                                                      .Dyes = {{.Field = dye}},
                                                      .RelaxationTarget = target,
                                                      .RowMetric = metric,
                                                      .WrapX = FluidWrap::Periodic,
                                                      .WrapY = FluidWrap::Clamped,
                                                      .TimeStep = 1.0f,
                                                      .JacobiIterations = 30,
                                                      .RelaxationRate = 1.0f,
                                                  });
    Context.ImmediateCommands([&](CommandBuffer& cmd) { sim->RecordSteps(cmd, Steps); });

    const FieldReader field = Read(Context, dye, Extent);
    auto PeakColumn = [&](const u32 row)
    {
        f32 best = -1.0f;
        u32 at = 0;
        for (u32 x = 0; x < Extent.x; ++x)
        {
            const f32 value = field.Scalar(x, row);
            if (value > best)
            {
                best = value;
                at = x;
            }
        }
        return at;
    };

    const u32 unstretched = PeakColumn(Extent.y / 4);
    const u32 stretched = PeakColumn((Extent.y * 3) / 4);
    CHECK(unstretched == StartColumn + Steps);
    CHECK(stretched == StartColumn + (Steps / 2));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "fluid sim: relaxation converges on its target at rate")
{
    AssetManager assets(Context, Tasks, Types);
    constexpr uvec2 Extent{16, 16};
    constexpr u32 Steps = 4;
    constexpr f32 Rate = 0.25f;

    auto Run = [&](const f32 rate)
    {
        const Ref<Image> velocity = MakeField(Context, "Jet Velocity", Extent, Format::RG32Sfloat);
        const Ref<Image> target = MakeField(Context, "Jet Target", Extent, Format::RG32Sfloat);
        SeedVelocity(velocity, Extent, [](u32, u32) { return vec2(0.0f); });
        SeedVelocity(target, Extent, [](u32, u32) { return vec2(1.0f, 0.0f); });

        const Unique<FluidSim> sim = FluidSim::Create(Context, assets,
                                                      {
                                                          .Name = "Jet",
                                                          .Velocity = velocity,
                                                          .RelaxationTarget = target,
                                                          .WrapX = FluidWrap::Periodic,
                                                          .WrapY = FluidWrap::Periodic,
                                                          .TimeStep = 1.0f,
                                                          .JacobiIterations = 8,
                                                          .RelaxationRate = rate,
                                                      });
        Context.ImmediateCommands([&](CommandBuffer& cmd) { sim->RecordSteps(cmd, Steps); });
        return Read(Context, velocity, Extent);
    };

    // A uniform field advects to itself and projects to itself, so the only thing moving the
    // velocity is the relaxation — and it moves by exactly the geometric law.
    const f32 expected = 1.0f - std::pow(1.0f - Rate, static_cast<f32>(Steps));
    const FieldReader relaxed = Run(Rate);
    for (const uvec2 probe : {uvec2{0, 0}, uvec2{7, 9}, uvec2{15, 15}})
    {
        const vec2 velocity = relaxed.Velocity(probe.x, probe.y);
        CHECK(velocity.x == doctest::Approx(expected).epsilon(0.001));
        CHECK(velocity.y == doctest::Approx(0.0f).epsilon(0.001));
    }

    const FieldReader untouched = Run(0.0f);
    CHECK(untouched.Velocity(7, 9).x == doctest::Approx(0.0f).epsilon(0.001));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "fluid sim: a fixed seed, config and step count hash to a pinned digest")
{
    AssetManager assets(Context, Tasks, Types);
    constexpr uvec2 Extent{32, 32};
    constexpr u32 Steps = 12;

    auto Run = [&]()
    {
        const Ref<Image> velocity =
            MakeField(Context, "Digest Velocity", Extent, Format::RG32Sfloat);
        const Ref<Image> dye = MakeField(Context, "Digest Dye", Extent, Format::RGBA16Sfloat);
        const Ref<Image> damping = MakeField(Context, "Digest Damping", Extent, Format::R16Sfloat);

        SeedVelocity(velocity, Extent,
                     [](const u32 x, const u32 y)
                     {
                         const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(Extent.x);
                         const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(Extent.y);
                         return vec2(0.9f * std::cos(6.2831853f * v),
                                     0.4f * std::sin(6.2831853f * u));
                     });
        SeedDye(damping, Extent,
                [](u32, const u32 y) { return y < 2 || y > Extent.y - 3 ? 0.75f : 1.0f; });

        std::vector<u16> dyeTexels(static_cast<usize>(Extent.x) * Extent.y * 4);
        for (u32 y = 0; y < Extent.y; ++y)
        {
            for (u32 x = 0; x < Extent.x; ++x)
            {
                const usize base = (static_cast<usize>(y) * Extent.x + x) * 4;
                dyeTexels[base + 0] = static_cast<u16>(glm::packHalf1x16((x % 8) * 0.125f));
                dyeTexels[base + 1] = static_cast<u16>(glm::packHalf1x16((y % 8) * 0.125f));
                dyeTexels[base + 2] = static_cast<u16>(glm::packHalf1x16(((x + y) % 5) * 0.2f));
                dyeTexels[base + 3] = static_cast<u16>(glm::packHalf1x16(1.0f));
            }
        }
        dye->UploadSync(std::span(reinterpret_cast<const u8*>(dyeTexels.data()),
                                  dyeTexels.size() * sizeof(u16)));

        vector<f32> metric(Extent.y, 1.0f);
        for (u32 y = 0; y < Extent.y; ++y)
        {
            metric[y] = 0.6f + (0.4f * static_cast<f32>(y) / static_cast<f32>(Extent.y - 1));
        }

        const Unique<FluidSim> sim =
            FluidSim::Create(Context, assets,
                             {
                                 .Name = "Digest",
                                 .Velocity = velocity,
                                 .Dyes = {{.Field = dye, .Dissipation = 0.01f}},
                                 .DampingMask = damping,
                                 .RowMetric = metric,
                                 .WrapX = FluidWrap::Periodic,
                                 .WrapY = FluidWrap::Clamped,
                                 .TimeStep = 0.75f,
                                 .JacobiIterations = 24,
                                 .VorticityConfinement = 0.4f,
                                 .VelocityDissipation = 0.0f,
                             });
        Context.ImmediateCommands([&](CommandBuffer& cmd) { sim->RecordSteps(cmd, Steps); });

        std::vector<u8> bytes = ReadField(Context, velocity);
        const std::vector<u8> dyeBytes = ReadField(Context, dye);
        bytes.insert(bytes.end(), dyeBytes.begin(), dyeBytes.end());
        return Digest(bytes);
    };

    const u64 first = Run();
    // Same seed, same config, same step count — the solver is a pure function of its inputs, so
    // a second run must land on the same bytes.
    CHECK(Run() == first);

    // Pinned on the reference host (Apple M2, MoltenVK). Cross-device bit-identity is not
    // promised and is not asserted anywhere; this is a change detector, re-pinned freely and
    // without ceremony on an intended solver change, with a note saying what moved.
    CHECK(first == 0xAF56CC176C83DA22ull);
}
