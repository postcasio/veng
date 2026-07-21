// Draw-grouping unit cases. GroupContiguousSlots collapses a submission-ordered
// span of draw slots into the contiguous runs that share both a source mesh and a
// pipeline, so each run binds the mesh's buffers and the material pipeline once.
// Pure data → data; no Context, no driver — the mesh and material pointers are
// never dereferenced, only compared, so opaque stand-in addresses drive the cases.

#include <doctest/doctest.h>

#include <span>

#include "Renderer/DrawGather.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Distinct, never-dereferenced stand-ins for the two identities a run is keyed on.
    // Grouping compares pointers only, so any distinct addresses do.
    int g_MeshA = 0;
    int g_MeshB = 0;
    int g_PipelineA = 0;
    int g_PipelineB = 0;

    const Mesh* MeshA = reinterpret_cast<const Mesh*>(&g_MeshA);
    const Mesh* MeshB = reinterpret_cast<const Mesh*>(&g_MeshB);
    const MaterialInstance* PipelineA = reinterpret_cast<const MaterialInstance*>(&g_PipelineA);
    const MaterialInstance* PipelineB = reinterpret_cast<const MaterialInstance*>(&g_PipelineB);

    DrawSlot Slot(const Mesh* mesh, const MaterialInstance* pipeline, const u32 candidateId)
    {
        return DrawSlot{
            .SourceMesh = mesh,
            .Pipeline = pipeline,
            .IndexCount = 3,
            .FirstIndex = 0,
            .VertexOffset = 0,
            .CandidateId = candidateId,
        };
    }
}

TEST_CASE("draw grouping: empty input produces no groups")
{
    vector<DrawGroup> groups;
    GroupContiguousSlots({}, groups);
    CHECK(groups.empty());
}

TEST_CASE("draw grouping: a single slot is one group covering it")
{
    const vector<DrawSlot> slots{Slot(MeshA, PipelineA, 0)};
    vector<DrawGroup> groups;
    GroupContiguousSlots(slots, groups);

    REQUIRE(groups.size() == 1);
    CHECK(groups[0].SourceMesh == MeshA);
    CHECK(groups[0].PipelineMaterial == PipelineA);
    CHECK(groups[0].FirstSlot == 0);
    CHECK(groups[0].SlotCount == 1);
}

TEST_CASE("draw grouping: contiguous slots sharing mesh and pipeline merge into one group")
{
    const vector<DrawSlot> slots{
        Slot(MeshA, PipelineA, 0),
        Slot(MeshA, PipelineA, 1),
        Slot(MeshA, PipelineA, 2),
    };
    vector<DrawGroup> groups;
    GroupContiguousSlots(slots, groups);

    REQUIRE(groups.size() == 1);
    CHECK(groups[0].FirstSlot == 0);
    CHECK(groups[0].SlotCount == 3);
}

TEST_CASE("draw grouping: a mesh change splits the run")
{
    const vector<DrawSlot> slots{
        Slot(MeshA, PipelineA, 0),
        Slot(MeshB, PipelineA, 1),
        Slot(MeshB, PipelineA, 2),
    };
    vector<DrawGroup> groups;
    GroupContiguousSlots(slots, groups);

    REQUIRE(groups.size() == 2);
    CHECK(groups[0].SourceMesh == MeshA);
    CHECK(groups[0].FirstSlot == 0);
    CHECK(groups[0].SlotCount == 1);
    CHECK(groups[1].SourceMesh == MeshB);
    CHECK(groups[1].FirstSlot == 1);
    CHECK(groups[1].SlotCount == 2);
}

TEST_CASE("draw grouping: a pipeline change splits the run even on one mesh")
{
    // The rule the split exists for: surface materials with different fragment shaders
    // share a mesh but not a pipeline, so each run must bind its own.
    const vector<DrawSlot> slots{
        Slot(MeshA, PipelineA, 0),
        Slot(MeshA, PipelineB, 1),
        Slot(MeshA, PipelineA, 2),
    };
    vector<DrawGroup> groups;
    GroupContiguousSlots(slots, groups);

    REQUIRE(groups.size() == 3);
    CHECK(groups[0].PipelineMaterial == PipelineA);
    CHECK(groups[0].SlotCount == 1);
    CHECK(groups[1].PipelineMaterial == PipelineB);
    CHECK(groups[1].FirstSlot == 1);
    CHECK(groups[1].SlotCount == 1);
    CHECK(groups[2].PipelineMaterial == PipelineA);
    CHECK(groups[2].FirstSlot == 2);
    CHECK(groups[2].SlotCount == 1);
}

TEST_CASE("draw grouping: the groups tile the slot range exactly once")
{
    // The invariant the geometry pass relies on: every slot is covered by exactly one
    // group, and the groups are in ascending slot order with no gap.
    const vector<DrawSlot> slots{
        Slot(MeshA, PipelineA, 0), Slot(MeshA, PipelineA, 1), Slot(MeshB, PipelineA, 2),
        Slot(MeshB, PipelineB, 3), Slot(MeshB, PipelineB, 4), Slot(MeshA, PipelineB, 5),
    };
    vector<DrawGroup> groups;
    GroupContiguousSlots(slots, groups);

    REQUIRE(groups.size() == 4);
    u32 expectedFirst = 0;
    for (const DrawGroup& group : groups)
    {
        CHECK(group.FirstSlot == expectedFirst);
        CHECK(group.SlotCount > 0);
        expectedFirst += group.SlotCount;
    }
    CHECK(expectedFirst == slots.size());
}

TEST_CASE("draw grouping: a non-adjacent repeat does not merge with an earlier run")
{
    // Grouping is contiguity-based, not a sort: the same mesh appearing again after an
    // intervening one yields a second group, never a merge back into the first.
    const vector<DrawSlot> slots{
        Slot(MeshA, PipelineA, 0),
        Slot(MeshB, PipelineA, 1),
        Slot(MeshA, PipelineA, 2),
    };
    vector<DrawGroup> groups;
    GroupContiguousSlots(slots, groups);

    REQUIRE(groups.size() == 3);
    CHECK(groups[0].SourceMesh == MeshA);
    CHECK(groups[1].SourceMesh == MeshB);
    CHECK(groups[2].SourceMesh == MeshA);
    CHECK(groups[2].FirstSlot == 2);
}

TEST_CASE("draw grouping: groups append to a non-empty output")
{
    // Both call sites pass a plan's freshly-cleared group vector, but the contract is
    // append: an existing entry is preserved and the new runs follow it.
    vector<DrawGroup> groups{DrawGroup{
        .SourceMesh = MeshB, .PipelineMaterial = PipelineB, .FirstSlot = 7, .SlotCount = 1}};
    const vector<DrawSlot> slots{Slot(MeshA, PipelineA, 0)};
    GroupContiguousSlots(slots, groups);

    REQUIRE(groups.size() == 2);
    CHECK(groups[0].FirstSlot == 7);
    CHECK(groups[1].FirstSlot == 0);
    CHECK(groups[1].SourceMesh == MeshA);
}
