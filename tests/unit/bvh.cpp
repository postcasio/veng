// BVH build + query: pure CPU math, no Context, no Vulkan symbol touched. The
// core property is equivalence — Query returns exactly the linear Intersects scan
// of the tight leaf boxes — plus a tree-validity invariant the checker asserts
// after each Build, both provable with no ICD (the frustum.cpp / bounds.cpp
// pattern). Build quality is pinned by a balance bound.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <limits>
#include <random>

// Veng.h (pulled by these) sets the engine's GLM config — Vulkan ZO depth, the
// Y-flip handedness — and must precede any bare glm include so glm compiles with
// it. The frustum extraction the equivalence test queries relies on it.
#include <Veng/Math/AABB.h>
#include <Veng/Math/BVH.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Scene/Camera.h>

#include <glm/gtc/matrix_transform.hpp>

using namespace Veng;

namespace
{
    AABB BoxAt(const vec3& center, f32 halfExtent)
    {
        return AABB{.Min = center - vec3(halfExtent), .Max = center + vec3(halfExtent)};
    }

    // The exact set Query must reproduce: every leaf whose tight box passes the
    // same Intersects the BVH accepts on.
    vector<u32> LinearScan(const vector<BVH::Leaf>& leaves, const Frustum& frustum)
    {
        vector<u32> ids;
        for (const BVH::Leaf& leaf : leaves)
        {
            if (Intersects(frustum, leaf.Box))
            {
                ids.push_back(leaf.Id);
            }
        }
        std::ranges::sort(ids);
        return ids;
    }

    vector<u32> SortedQuery(const BVH& bvh, const Frustum& frustum)
    {
        vector<u32> ids;
        bvh.Query(frustum, ids);
        std::ranges::sort(ids);
        return ids;
    }

    // Recursive validity walk over the index pool, reached through the public
    // accessors plus a re-query: every internal node's box contains both
    // children's boxes; every leaf id is reached exactly once; node height is
    // 1 + max(child heights). The pool layout is private, so the checker
    // reconstructs the invariants the contract guarantees from the outside —
    // GetLeafCount/GetNodeCount/GetHeight and the equivalence of a frustum
    // containing everything (every leaf reachable exactly once).
    void CheckValidity(const BVH& bvh, const vector<BVH::Leaf>& leaves)
    {
        CHECK(bvh.GetLeafCount() == static_cast<u32>(leaves.size()));

        if (leaves.empty())
        {
            CHECK(bvh.GetNodeCount() == 0);
            CHECK(bvh.GetHeight() == 0);
            CHECK(bvh.GetRootBounds().IsEmpty());
            return;
        }

        // A tree over N leaves is a full binary tree: N leaves + (N - 1) internal
        // nodes = 2N - 1 nodes.
        CHECK(bvh.GetNodeCount() == 2u * static_cast<u32>(leaves.size()) - 1u);

        // The root bounds enclose every leaf box (a node's box contains its
        // children's, transitively the whole set).
        AABB expected = AABB::Empty();
        for (const BVH::Leaf& leaf : leaves)
        {
            expected.Expand(leaf.Box);
        }
        const AABB root = bvh.GetRootBounds();
        CHECK(root.Min.x <= doctest::Approx(expected.Min.x));
        CHECK(root.Min.y <= doctest::Approx(expected.Min.y));
        CHECK(root.Min.z <= doctest::Approx(expected.Min.z));
        CHECK(root.Max.x >= doctest::Approx(expected.Max.x));
        CHECK(root.Max.y >= doctest::Approx(expected.Max.y));
        CHECK(root.Max.z >= doctest::Approx(expected.Max.z));

        // A single leaf has height 0; any larger tree's height is positive and
        // bounded by N - 1 (a fully-degenerate chain).
        const i32 height = bvh.GetHeight();
        if (leaves.size() == 1)
        {
            CHECK(height == 0);
        }
        else
        {
            CHECK(height >= 1);
            CHECK(height <= static_cast<i32>(leaves.size()) - 1);
        }

        // A frustum that swallows the whole root reaches every leaf exactly once:
        // the returned id multiset is precisely the input ids, no duplicate, no
        // omission — the "every leaf reachable from root exactly once" invariant.
        const vec3 size = glm::max(root.Size(), vec3(1.0f));
        const mat4 wide =
            glm::ortho(root.Min.x - size.x, root.Max.x + size.x, root.Min.y - size.y,
                       root.Max.y + size.y, -(root.Max.z + size.z), -(root.Min.z - size.z));
        // glm::ortho maps a right-handed eye space; feeding world coords with a
        // generous z range keeps every leaf inside. The far/near are negated to
        // span the world z extent under the look-down-(-z) convention.
        vector<u32> reached;
        bvh.Query(Frustum::FromViewProjection(wide), reached);
        std::ranges::sort(reached);

        vector<u32> all;
        for (const BVH::Leaf& leaf : leaves)
        {
            all.push_back(leaf.Id);
        }
        std::ranges::sort(all);

        CHECK(reached == all); // each leaf exactly once — no dup, no drop
    }

    CameraView MakeCameraAt(const vec3& eye, const vec3& target)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.5f, 500.0f);
        camera.SetView(eye, target, vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }
}

TEST_CASE("BVH equivalence: Query == linear tight scan over randomized builds and frustums")
{
    std::mt19937 rng(0xB57Au);
    std::uniform_real_distribution<f32> pos(-50.0f, 50.0f);
    std::uniform_real_distribution<f32> half(0.1f, 4.0f);
    std::uniform_int_distribution<i32> countDist(1, 200);

    // A bank of frustums each randomized build is queried against — randomized
    // cameras looking from varied eyes at varied targets, plus axis-aligned ortho
    // and degenerate (very narrow / very wide) ones.
    const auto MakeFrustums = [&]()
    {
        vector<Frustum> frustums;
        for (i32 i = 0; i < 12; ++i)
        {
            const vec3 eye(pos(rng), pos(rng), pos(rng));
            const vec3 target(pos(rng), pos(rng), pos(rng));
            if (glm::distance(eye, target) < 1.0f)
            {
                continue; // a zero look vector is undefined; skip it
            }
            frustums.push_back(
                Frustum::FromViewProjection(MakeCameraAt(eye, target).ViewProjection()));
        }
        // Axis-aligned ortho slab.
        frustums.push_back(
            Frustum::FromViewProjection(glm::ortho(-20.0f, 20.0f, -20.0f, 20.0f, 1.0f, 80.0f)));
        // Degenerate: a razor-thin perspective and an enormous one.
        frustums.push_back(Frustum::FromViewProjection(
            MakeCameraAt(vec3(0.0f, 0.0f, 60.0f), vec3(0.0f)).ViewProjection()));
        CameraView narrow;
        narrow.SetPerspective(glm::radians(2.0f), 1.0f, 0.5f, 500.0f);
        narrow.SetView(vec3(0.0f, 0.0f, 80.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        frustums.push_back(Frustum::FromViewProjection(narrow.ViewProjection()));
        return frustums;
    };

    for (i32 build = 0; build < 60; ++build)
    {
        const i32 count = countDist(rng);
        vector<BVH::Leaf> leaves;
        leaves.reserve(static_cast<usize>(count));
        for (i32 i = 0; i < count; ++i)
        {
            leaves.push_back(BVH::Leaf{.Box = BoxAt(vec3(pos(rng), pos(rng), pos(rng)), half(rng)),
                                       .Id = static_cast<u32>(i)});
        }

        BVH bvh;
        bvh.Build(leaves);
        CheckValidity(bvh, leaves);

        for (const Frustum& frustum : MakeFrustums())
        {
            CHECK(SortedQuery(bvh, frustum) == LinearScan(leaves, frustum));
        }
    }
}

TEST_CASE("BVH all-in / all-out / empty / single")
{
    // Empty build — empty tree, empty query, Empty() root bounds.
    {
        BVH bvh;
        bvh.Build({});
        CHECK(bvh.GetLeafCount() == 0);
        CHECK(bvh.GetNodeCount() == 0);
        CHECK(bvh.GetHeight() == 0);
        CHECK(bvh.GetRootBounds().IsEmpty());

        vector<u32> ids;
        bvh.Query(Frustum::FromViewProjection(glm::ortho(-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f)),
                  ids);
        CHECK(ids.empty());
    }

    // A clustered set so a single frustum can contain or miss them all.
    vector<BVH::Leaf> leaves;
    for (i32 i = 0; i < 30; ++i)
    {
        leaves.push_back(BVH::Leaf{
            .Box =
                BoxAt(vec3(static_cast<f32>(i % 5), static_cast<f32>((i / 5) % 5), -10.0f), 0.25f),
            .Id = static_cast<u32>(i)});
    }
    BVH bvh;
    bvh.Build(leaves);
    CheckValidity(bvh, leaves);

    // All-in: a wide ortho swallowing the cluster returns every id.
    const Frustum wide =
        Frustum::FromViewProjection(glm::ortho(-100.0f, 100.0f, -100.0f, 100.0f, 1.0f, 100.0f));
    CHECK(SortedQuery(bvh, wide) == LinearScan(leaves, wide));
    CHECK(SortedQuery(bvh, wide).size() == leaves.size());

    // All-out: an ortho box far from the cluster returns nothing.
    const Frustum elsewhere =
        Frustum::FromViewProjection(glm::ortho(900.0f, 901.0f, 900.0f, 901.0f, 1.0f, 100.0f));
    CHECK(SortedQuery(bvh, elsewhere).empty());

    // Single leaf — Query matches the scan whether the box is inside the frustum
    // or just outside it (the boundary either way).
    {
        BVH one;
        const vector<BVH::Leaf> single{
            BVH::Leaf{.Box = BoxAt(vec3(0.0f, 0.0f, -10.0f), 0.5f), .Id = 7u}};
        one.Build(single);
        CheckValidity(one, single);
        CHECK(one.GetHeight() == 0);

        const Frustum hit = Frustum::FromViewProjection(
            MakeCameraAt(vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 0.0f, -1.0f)).ViewProjection());
        CHECK(SortedQuery(one, hit) == LinearScan(single, hit));

        const Frustum miss =
            Frustum::FromViewProjection(glm::ortho(50.0f, 51.0f, 50.0f, 51.0f, 1.0f, 10.0f));
        CHECK(SortedQuery(one, miss) == LinearScan(single, miss));
        CHECK(SortedQuery(one, miss).empty());
    }
}

TEST_CASE("BVH balance: height stays within a small factor of log2(leaf count)")
{
    std::mt19937 rng(0x1234u);
    std::uniform_real_distribution<f32> pos(-200.0f, 200.0f);
    std::uniform_real_distribution<f32> half(0.1f, 2.0f);

    for (const i32 count : {64, 256, 1024, 4096})
    {
        vector<BVH::Leaf> leaves;
        leaves.reserve(static_cast<usize>(count));
        for (i32 i = 0; i < count; ++i)
        {
            leaves.push_back(BVH::Leaf{.Box = BoxAt(vec3(pos(rng), pos(rng), pos(rng)), half(rng)),
                                       .Id = static_cast<u32>(i)});
        }

        BVH bvh;
        bvh.Build(leaves);
        CheckValidity(bvh, leaves);

        const i32 height = bvh.GetHeight();
        const f32 ideal = std::log2(static_cast<f32>(count));
        // A median/SAH top-down split over a random uniform set yields a
        // near-logarithmic tree; allow a generous 4x factor (+2) so a chance
        // imbalance never flakes while a linear-chain regression still trips.
        CHECK(static_cast<f32>(height) <= 4.0f * ideal + 2.0f);
    }
}

TEST_CASE("BVH bucket split: the linear sweep picks a least-cost boundary")
{
    // A fixed, uneven bucket set: empty buckets at both ends and in the middle, boxes of different
    // sizes, so the least-cost boundary is neither the median nor the first non-empty one.
    std::array<AABB, BVH::BucketCount> boxes;
    std::array<u32, BVH::BucketCount> counts = {0, 3, 1, 0, 7, 2, 0, 0, 5, 1, 4, 0};
    boxes.fill(AABB::Empty());
    for (i32 b = 0; b < BVH::BucketCount; ++b)
    {
        if (counts[b] > 0)
        {
            const f32 x = static_cast<f32>(b) * 4.0f;
            boxes[b] = AABB{.Min = vec3(x, -1.0f - static_cast<f32>(b % 3), -1.0f),
                            .Max = vec3(x + 3.0f, 1.0f, 1.0f + static_cast<f32>(b % 4))};
        }
    }

    const Detail::BucketSplit chosen = Detail::BestBucketSplit(boxes, counts);
    REQUIRE(chosen.Split > 0);
    REQUIRE(chosen.Split < BVH::BucketCount);

    // The property is "minimum cost": cost every boundary that leaves both sides non-empty from
    // its definition, and the chosen one is the least of them and costed as such.
    const auto area = [](const AABB& box)
    {
        const vec3 size = glm::max(box.Size(), vec3(0.0f));
        return 2.0f * (size.x * size.y + size.y * size.z + size.z * size.x);
    };
    f32 least = std::numeric_limits<f32>::infinity();
    for (i32 split = 1; split < BVH::BucketCount; ++split)
    {
        AABB left = AABB::Empty();
        AABB right = AABB::Empty();
        u32 leftCount = 0;
        u32 rightCount = 0;
        for (i32 b = 0; b < BVH::BucketCount; ++b)
        {
            (b < split ? left : right).Expand(boxes[b]);
            (b < split ? leftCount : rightCount) += counts[b];
        }
        if (leftCount > 0 && rightCount > 0)
        {
            least = std::min(least, area(left) * static_cast<f32>(leftCount) +
                                        area(right) * static_cast<f32>(rightCount));
        }
    }
    CHECK(chosen.Cost == doctest::Approx(least));

    // Every leaf in one bucket leaves no boundary to choose.
    std::array<u32, BVH::BucketCount> single = {};
    single[5] = 9;
    CHECK(Detail::BestBucketSplit(boxes, single).Split == -1);
}

TEST_CASE("BVH refit: queries stay exact over moved leaves, and scrambling them degrades the cost")
{
    // Sixteen boxes along x: the build groups neighbours, so its cost is low.
    vector<BVH::Leaf> leaves;
    for (u32 i = 0; i < 16; ++i)
    {
        leaves.push_back(
            BVH::Leaf{.Box = BoxAt(vec3(static_cast<f32>(i) * 10.0f, 0.0f, 0.0f), 1.0f), .Id = i});
    }
    BVH bvh;
    bvh.Build(leaves);
    CHECK(bvh.GetCost() == doctest::Approx(bvh.GetBuildCost()));

    // Move one leaf a little: the refit tree still queries exactly the linear scan of the new
    // boxes, and its root covers the moved leaf.
    vector<AABB> boxes;
    for (const BVH::Leaf& leaf : leaves)
    {
        boxes.push_back(leaf.Box);
    }
    boxes[3] = BoxAt(vec3(35.0f, 20.0f, 0.0f), 1.0f);
    leaves[3].Box = boxes[3];
    bvh.Refit(boxes);
    CheckValidity(bvh, leaves);

    std::mt19937 rng(0x5EEDu);
    std::uniform_real_distribution<f32> pos(-60.0f, 200.0f);
    for (i32 i = 0; i < 8; ++i)
    {
        const vec3 eye(pos(rng), pos(rng), pos(rng));
        const vec3 target(pos(rng), 0.0f, 0.0f);
        if (glm::distance(eye, target) < 1.0f)
        {
            continue;
        }
        const Frustum frustum =
            Frustum::FromViewProjection(MakeCameraAt(eye, target).ViewProjection());
        CHECK(SortedQuery(bvh, frustum) == LinearScan(leaves, frustum));
    }

    // Scramble every leaf across the line: each internal node now spans most of it, so the refit
    // tree's cost rises well past the cost it recorded at build, which stays put.
    const f32 buildCost = bvh.GetBuildCost();
    for (u32 i = 0; i < 16; ++i)
    {
        boxes[i] = BoxAt(vec3(static_cast<f32>((i * 7) % 16) * 10.0f, 0.0f, 0.0f), 1.0f);
    }
    bvh.Refit(boxes);
    CHECK(bvh.GetBuildCost() == buildCost);
    CHECK(bvh.GetCost() > 1.5f * buildCost);
}
