// Draw-grouping unit cases. GroupContiguousSlots collapses a submission-ordered
// span of draw slots into the contiguous runs that share both a source mesh and a
// pipeline, so each run binds the mesh's buffers and the material pipeline once, and
// splits each group into the instanced runs one DrawIndexed covers. SortDrawKeys is the
// ordering that makes equal draws adjacent before slots are claimed.
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

    // A slot drawing the given submesh's index range; the stand-in pipeline doubles as its key.
    DrawSlot Slot(const Mesh* mesh, const MaterialInstance* pipeline, const u32 candidateId,
                  const u32 subMesh = 0)
    {
        return DrawSlot{
            .SourceMesh = mesh,
            .Pipeline = pipeline,
            .PipelineKey = pipeline,
            .IndexCount = 3,
            .FirstIndex = subMesh * 3,
            .VertexOffset = 0,
            .CandidateId = candidateId,
        };
    }

    struct Grouped
    {
        vector<DrawGroup> Groups;
        vector<InstanceRun> Runs;
    };

    Grouped Group(const std::span<const DrawSlot> slots)
    {
        Grouped out;
        GroupContiguousSlots(slots, out.Groups, out.Runs);
        return out;
    }
}

TEST_CASE("draw grouping: empty input produces no groups")
{
    const Grouped out = Group({});
    CHECK(out.Groups.empty());
    CHECK(out.Runs.empty());
}

TEST_CASE("draw grouping: a single slot is one group covering it")
{
    const vector<DrawSlot> slots{Slot(MeshA, PipelineA, 0)};
    const vector<DrawGroup> groups = Group(slots).Groups;

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
    const vector<DrawGroup> groups = Group(slots).Groups;

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
    const vector<DrawGroup> groups = Group(slots).Groups;

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
    const vector<DrawGroup> groups = Group(slots).Groups;

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
    const vector<DrawGroup> groups = Group(slots).Groups;

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
    // Grouping is contiguity-based, not a sort (SortDrawKeys orders the draws before the
    // slots are claimed): the same mesh appearing again after an intervening one yields a
    // second group, never a merge back into the first.
    const vector<DrawSlot> slots{
        Slot(MeshA, PipelineA, 0),
        Slot(MeshB, PipelineA, 1),
        Slot(MeshA, PipelineA, 2),
    };
    const vector<DrawGroup> groups = Group(slots).Groups;

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
    vector<DrawGroup> groups{DrawGroup{.SourceMesh = MeshB,
                                       .PipelineMaterial = PipelineB,
                                       .FirstSlot = 7,
                                       .SlotCount = 1,
                                       .FirstRun = 0,
                                       .RunCount = 1}};
    vector<InstanceRun> runs{InstanceRun{.FirstSlot = 7, .Count = 1}};
    const vector<DrawSlot> slots{Slot(MeshA, PipelineA, 0)};
    GroupContiguousSlots(slots, groups, runs);

    REQUIRE(groups.size() == 2);
    CHECK(groups[0].FirstSlot == 7);
    CHECK(groups[1].FirstSlot == 0);
    CHECK(groups[1].SourceMesh == MeshA);
    REQUIRE(runs.size() == 2);
    CHECK(groups[1].FirstRun == 1);
    CHECK(groups[1].RunCount == 1);
}

TEST_CASE("draw grouping: equal slots form one instanced run, and a submesh change splits it")
{
    // Within one (mesh, pipeline) group, a run is a stretch drawing the same index range with
    // consecutive candidate ids — one instanced draw. A different submesh starts a new run.
    const vector<DrawSlot> slots{
        Slot(MeshA, PipelineA, 0, 0), Slot(MeshA, PipelineA, 1, 0), Slot(MeshA, PipelineA, 2, 0),
        Slot(MeshA, PipelineA, 3, 1), Slot(MeshA, PipelineA, 4, 1),
    };
    const Grouped out = Group(slots);

    REQUIRE(out.Groups.size() == 1);
    CHECK(out.Groups[0].FirstRun == 0);
    REQUIRE(out.Groups[0].RunCount == 2);
    REQUIRE(out.Runs.size() == 2);
    CHECK(out.Runs[0].FirstSlot == 0);
    CHECK(out.Runs[0].Count == 3);
    CHECK(out.Runs[1].FirstSlot == 3);
    CHECK(out.Runs[1].Count == 2);
}

TEST_CASE("draw grouping: a gap in the candidate ids splits a run")
{
    // An instanced draw reads candidates firstInstance, firstInstance + 1, ...; slots whose ids
    // are not consecutive cannot share one, even drawing the same range.
    const vector<DrawSlot> slots{Slot(MeshA, PipelineA, 0), Slot(MeshA, PipelineA, 2)};
    const Grouped out = Group(slots);

    REQUIRE(out.Groups.size() == 1);
    CHECK(out.Groups[0].RunCount == 2);
}

TEST_CASE("draw ordering: survivors of two meshes interleaved in gather order lay out as two "
          "instanced runs")
{
    // The batching claim: gather order alternates meshes, which contiguity alone would draw as
    // one group per survivor. Sorting the keys before claiming slots makes each mesh's survivors
    // adjacent, so each mesh is one group with one run covering exactly its slots.
    vector<DrawKey> keys;
    for (u32 candidate = 0; candidate < 8; ++candidate)
    {
        keys.push_back(DrawKey{.Pipeline = PipelineA,
                               .SourceMesh = candidate % 2 == 0 ? MeshA : MeshB,
                               .SubMeshIndex = 0,
                               .Candidate = candidate});
    }
    SortDrawKeys(keys);

    // Claim slots in key order, exactly as the static gather does: slot == position.
    vector<DrawSlot> slots;
    for (u32 slot = 0; slot < keys.size(); ++slot)
    {
        slots.push_back(Slot(keys[slot].SourceMesh, PipelineA, slot));
    }
    const Grouped out = Group(slots);

    REQUIRE(out.Groups.size() == 2);
    REQUIRE(out.Runs.size() == 2);
    u32 nextSlot = 0;
    for (const DrawGroup& group : out.Groups)
    {
        REQUIRE(group.RunCount == 1);
        const InstanceRun& run = out.Runs[group.FirstRun];
        // Contiguous slot ranges, tiling the layout from 0.
        CHECK(run.FirstSlot == nextSlot);
        CHECK(run.Count == 4);
        nextSlot += run.Count;

        // Exactly this mesh's survivors, still in gather order within the run.
        u32 previousCandidate = 0;
        for (u32 s = run.FirstSlot; s < run.FirstSlot + run.Count; ++s)
        {
            CHECK(keys[s].SourceMesh == group.SourceMesh);
            if (s > run.FirstSlot)
            {
                CHECK(keys[s].Candidate > previousCandidate);
            }
            previousCandidate = keys[s].Candidate;
        }
    }
    CHECK(nextSlot == keys.size());
    CHECK(out.Groups[0].SourceMesh != out.Groups[1].SourceMesh);
}

TEST_CASE("draw ordering: a pipeline's draws are adjacent whatever their meshes")
{
    // The pipeline is the outer key: each pipeline binds once, and its meshes sort within it.
    vector<DrawKey> keys{
        {.Pipeline = PipelineA, .SourceMesh = MeshA, .SubMeshIndex = 0, .Candidate = 0},
        {.Pipeline = PipelineB, .SourceMesh = MeshA, .SubMeshIndex = 0, .Candidate = 1},
        {.Pipeline = PipelineA, .SourceMesh = MeshB, .SubMeshIndex = 0, .Candidate = 2},
        {.Pipeline = PipelineB, .SourceMesh = MeshB, .SubMeshIndex = 0, .Candidate = 3},
        {.Pipeline = PipelineA, .SourceMesh = MeshA, .SubMeshIndex = 1, .Candidate = 4},
    };
    SortDrawKeys(keys);

    u32 pipelineChanges = 0;
    for (usize i = 1; i < keys.size(); ++i)
    {
        pipelineChanges += keys[i].Pipeline != keys[i - 1].Pipeline ? 1 : 0;
    }
    CHECK(pipelineChanges == 1);
}
