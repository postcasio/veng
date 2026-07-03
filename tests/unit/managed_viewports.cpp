// Managed-viewport-list layout resolution: the pure CPU math the engine's
// managed viewport set uses to resolve each normalized ViewportLayout to a pixel
// region (round(Layout · render extent)) at construction and on every swapchain
// resize. No Context, no Vulkan symbol touched — the resolution is mirrored here
// exactly as Application::ResolveManagedRegion computes it, so the quadrant
// split, resize tracking, and reconfigure-back-to-one are exercised headless
// (the device-owning path is proven by the gpu splitscreen test).

#include <doctest/doctest.h>

#include <Veng/Application.h>
#include <Veng/Renderer/ViewportRegion.h>

#include <glm/glm.hpp>

#include <span>
#include <vector>

using namespace Veng;
using Veng::Renderer::ViewportRegion;

namespace
{
    // Application::ResolveManagedRegion, isolated so the suite needs no Context to
    // construct an Application. Mirrors the impl exactly: a pinned Extent is a
    // fixed region at the origin, otherwise round(Layout · render extent).
    ViewportRegion ResolveManagedRegion(const ManagedViewportInfo& info, uvec2 renderExtent)
    {
        if (info.Extent != uvec2{})
        {
            return {.Offset = {0, 0}, .Extent = info.Extent};
        }

        const vec2 extentF = vec2(renderExtent);
        const ivec2 offset = ivec2(glm::round(info.Layout.Offset * extentF));
        const uvec2 extent = uvec2(glm::round(info.Layout.Extent * extentF));
        return {.Offset = offset, .Extent = extent};
    }

    // The managed set the engine holds: one region per info, index 0 the primary,
    // each region re-resolved on a resize. Models the observable state of
    // Application::m_ManagedViewports across BuildManagedViewports / the resize
    // callback / a reconfigure — the region math without the GPU viewport.
    struct ManagedSet
    {
        std::vector<ManagedViewportInfo> Infos;
        std::vector<ViewportRegion> Regions;
        uvec2 RenderExtent;

        void Build(std::span<const ManagedViewportInfo> infos, uvec2 renderExtent)
        {
            RenderExtent = renderExtent;
            Infos.assign(infos.begin(), infos.end());
            Regions.clear();
            for (const ManagedViewportInfo& info : Infos)
            {
                Regions.push_back(ResolveManagedRegion(info, RenderExtent));
            }
        }

        // The swapchain-invalidation callback: re-resolve every window-tracking
        // viewport's region; a pinned viewport keeps its fixed extent.
        void Resize(uvec2 renderExtent)
        {
            RenderExtent = renderExtent;
            for (usize i = 0; i < Infos.size(); ++i)
            {
                if (Infos[i].Extent == uvec2{})
                {
                    Regions[i] = ResolveManagedRegion(Infos[i], RenderExtent);
                }
            }
        }
    };
}

TEST_CASE("A single managed viewport with the default Layout covers the whole window")
{
    // The plug-and-play default: one info, full-window Layout, byte-identical to
    // the delivered single managed viewport.
    ManagedSet set;
    const ManagedViewportInfo primary{};
    set.Build(std::span<const ManagedViewportInfo>(&primary, 1), {1280, 720});

    REQUIRE(set.Regions.size() == 1);
    CHECK(set.Regions[0].Offset == ivec2{0, 0});
    CHECK(set.Regions[0].Extent == uvec2{1280, 720});
}

TEST_CASE("Reconfiguring to two side-by-side quadrant Layouts resolves left/right pixel regions")
{
    ManagedSet set;
    const ManagedViewportInfo initial{};
    set.Build(std::span<const ManagedViewportInfo>(&initial, 1), {1280, 720});

    // Left half and right half — the split-screen reconfigure.
    ManagedViewportInfo left{};
    left.Layout = {.Offset = {0.0f, 0.0f}, .Extent = {0.5f, 1.0f}};
    ManagedViewportInfo right{};
    right.Layout = {.Offset = {0.5f, 0.0f}, .Extent = {0.5f, 1.0f}};
    const ManagedViewportInfo halves[] = {left, right};

    set.Build(halves, {1280, 720});

    REQUIRE(set.Regions.size() == 2);
    // Index 0 stays the primary (the left half).
    CHECK(set.Regions[0].Offset == ivec2{0, 0});
    CHECK(set.Regions[0].Extent == uvec2{640, 720});
    // The right half sits at x = 640.
    CHECK(set.Regions[1].Offset == ivec2{640, 0});
    CHECK(set.Regions[1].Extent == uvec2{640, 720});
}

TEST_CASE("Quadrant regions track their normalized Layouts across a swapchain resize")
{
    ManagedSet set;
    ManagedViewportInfo topLeft{};
    topLeft.Layout = {.Offset = {0.0f, 0.0f}, .Extent = {0.5f, 0.5f}};
    ManagedViewportInfo bottomRight{};
    bottomRight.Layout = {.Offset = {0.5f, 0.5f}, .Extent = {0.5f, 0.5f}};
    const ManagedViewportInfo quadrants[] = {topLeft, bottomRight};

    set.Build(quadrants, {1280, 720});
    REQUIRE(set.Regions.size() == 2);
    CHECK(set.Regions[0].Offset == ivec2{0, 0});
    CHECK(set.Regions[0].Extent == uvec2{640, 360});
    CHECK(set.Regions[1].Offset == ivec2{640, 360});
    CHECK(set.Regions[1].Extent == uvec2{640, 360});

    // Resize the swapchain: both regions re-resolve from their normalized Layouts,
    // no game code and no reconfigure.
    set.Resize({1920, 1080});
    CHECK(set.Regions[0].Offset == ivec2{0, 0});
    CHECK(set.Regions[0].Extent == uvec2{960, 540});
    CHECK(set.Regions[1].Offset == ivec2{960, 540});
    CHECK(set.Regions[1].Extent == uvec2{960, 540});
}

TEST_CASE("Reconfiguring back to one restores the primary full-window at index 0")
{
    ManagedSet set;

    ManagedViewportInfo left{};
    left.Layout = {.Offset = {0.0f, 0.0f}, .Extent = {0.5f, 1.0f}};
    ManagedViewportInfo right{};
    right.Layout = {.Offset = {0.5f, 0.0f}, .Extent = {0.5f, 1.0f}};
    const ManagedViewportInfo halves[] = {left, right};
    set.Build(halves, {1280, 720});
    REQUIRE(set.Regions.size() == 2);

    // Back to a single default-Layout viewport: index 0 is full-window again.
    const ManagedViewportInfo primary{};
    set.Build(std::span<const ManagedViewportInfo>(&primary, 1), {1280, 720});

    REQUIRE(set.Regions.size() == 1);
    CHECK(set.Regions[0].Offset == ivec2{0, 0});
    CHECK(set.Regions[0].Extent == uvec2{1280, 720});
}

TEST_CASE("A pinned Extent is a fixed region that does not track a resize")
{
    ManagedSet set;
    ManagedViewportInfo pinned{};
    pinned.Extent = {800, 600};
    set.Build(std::span<const ManagedViewportInfo>(&pinned, 1), {1280, 720});

    REQUIRE(set.Regions.size() == 1);
    CHECK(set.Regions[0].Offset == ivec2{0, 0});
    CHECK(set.Regions[0].Extent == uvec2{800, 600});

    // The pinned region is unmoved by a swapchain resize.
    set.Resize({1920, 1080});
    CHECK(set.Regions[0].Extent == uvec2{800, 600});
}
