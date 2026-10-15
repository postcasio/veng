#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Renderer/SceneRenderer.h>

#include "GpuBlocks.h"

namespace Veng
{
    class Mesh;
    class Material;
    class MaterialInstance;
}

namespace Veng::Renderer
{
    class Buffer;
    class DescriptorSet;

    // One per-candidate draw the geometry pass records, in candidate-slot order.
    // The candidate id (== the slot) reaches the surface vertex stage via the
    // instance attribute fetched at firstInstance + instance; its DrawData record holds the
    // world/normal/material. CPU mode issues one instanced DrawIndexed per run of equal slots;
    // GPU mode issues one DrawIndexedIndirect over a group's contiguous command run.
    struct DrawSlot
    {
        const Mesh* SourceMesh;
        // The submesh's material, bound for this slot's group. Its Bind binds the parent
        // material's pipeline (a Surface instance pushes nothing; its selector rides DrawData).
        const MaterialInstance* Pipeline;
        // The identity of the pipeline Pipeline binds — its parent material. Groups are keyed on
        // this, not on the instance, so instances of one parent share a group and a pipeline bind.
        const void* PipelineKey;
        u32 IndexCount;
        u32 FirstIndex;
        i32 VertexOffset;
        u32 CandidateId; // == the per-draw DrawData slot and the command firstInstance
    };

    // A contiguous run of slots drawing the same index range of the same mesh with consecutive
    // candidate ids, so one instanced DrawIndexed(IndexCount, Count, ..., firstInstance = the first
    // slot's candidate id) covers it: instance i reads candidate firstInstance + i.
    struct InstanceRun
    {
        u32 FirstSlot;
        u32 Count;
    };

    // A contiguous run of candidate slots sharing one source mesh and one pipeline, so the
    // mesh's vertex/index buffers and the material pipeline each bind once. CPU mode draws
    // each of the group's runs instanced; GPU mode issues one vkCmdDrawIndexedIndirect over the
    // group's commands (the culled slots no-op).
    struct DrawGroup
    {
        const Mesh* SourceMesh;
        // The material whose pipeline the group's draws are recorded through. Borrowed: the
        // mesh's resident AssetHandle keeps it alive for this frame.
        const MaterialInstance* PipelineMaterial;
        u32 FirstSlot;
        u32 SlotCount;
        // The group's instanced runs, a contiguous range of the plan's run list.
        u32 FirstRun;
        u32 RunCount;
    };

    // The per-frame submission plan SceneRenderer fills before each graph replay and
    // the geometry pass reads at record time. Held in SceneRenderer::Internal.
    struct GBufferDrawPlan
    {
        SceneRendererSettings::CullMode Cull = SceneRendererSettings::CullMode::CPU;
        SurfacePush Push;
        Ref<DescriptorSet> DrawDataSet;
        Ref<Buffer> CandidateIdBuffer;
        Ref<Buffer> IndirectBuffer;   // GPU mode only
        u32 IndirectRegionOffset = 0; // byte offset of this frame's command region (GPU)
        // A representative loaded static surface material (the first survivor's). Its shared
        // pipeline layout drives the picking-pipeline build; the g-buffer draws bind each
        // group's own pipeline (see DrawGroup). Borrowed: the mesh's resident AssetHandle keeps
        // it alive for this frame.
        const MaterialInstance* PipelineMaterial = nullptr;
        // Ordered by (parent pipeline, mesh, submesh) before slots were claimed, so equal draws
        // are adjacent and each run below is one instanced draw.
        vector<DrawSlot> Slots;
        // Contiguous runs sharing a mesh and a pipeline; each carries the material pipeline to
        // bind for its draws, so a scene mixing surface materials with different fragment
        // shaders binds each draw's own pipeline rather than one for the whole pass.
        vector<DrawGroup> Groups;
        // The groups' instanced runs; a group indexes its range.
        vector<InstanceRun> Runs;

        // Skinned draws ride a parallel CPU-direct path after the static draws: per group the
        // skinned surface pipeline (built from surface_skinned.vert) from that group's material,
        // with the per-instance palette bound at set 2. They share the same DrawData buffer
        // (each slot's DrawData.PaletteBase points into the palette). This is a representative
        // loaded skinned material whose shared layout drives the skinned picking-pipeline build.
        const MaterialInstance* SkinnedPipelineMaterial = nullptr;
        Ref<DescriptorSet> PaletteSet;
        vector<DrawSlot> SkinnedSlots;
        vector<DrawGroup> SkinnedGroups;
        vector<InstanceRun> SkinnedRuns;
    };

    // One forward translucent draw, in back-to-front order. Unlike the opaque plan's
    // pipeline-shared slots, each translucent draw binds its own material's pipeline (built
    // per parent by the pass against the HDR format), because translucent fragment shaders
    // differ. The draw reads its per-draw record (world/normal/material selector) from the
    // shared DrawData SSBO by CandidateId, exactly like a static surface draw, so it reuses the
    // renderer's DrawData / candidate-id buffers — its slots are allocated after the opaque and
    // skinned ones. Material is borrowed: the mesh's resident AssetHandle keeps it alive for
    // this frame. ViewDepth is the sort key: the submesh centre's view-space z, more negative
    // for a farther draw, which draws first.
    struct TranslucentDraw
    {
        const MaterialInstance* Material;
        const Mesh* SourceMesh;
        u32 IndexCount;
        u32 FirstIndex;
        u32 CandidateId;
        f32 ViewDepth;
        // The parent material's authored draw-order priority: draws sort back-to-front
        // within ascending priority groups, so a higher priority draws over a lower one.
        i32 SortPriority;
        // The screen rectangle the submesh's bounds project to, in UV (min xy, max xy), clamped
        // to [0, 1]; the whole screen when any corner lies behind the eye. What the segmenting
        // tests two draws' overlap by.
        vec4 ScreenRect{0.0f, 0.0f, 1.0f, 1.0f};
        // Whether the material opted into the reduced-resolution layer.
        bool HalfResolution = false;
    };

    // The per-frame forward translucent submission plan SceneRenderer fills before each graph
    // replay and the translucent pass reads at record time. Held in SceneRenderer::Internal.
    struct TranslucentDrawPlan
    {
        SurfacePush Push;
        Ref<DescriptorSet> DrawDataSet;
        Ref<Buffer> CandidateIdBuffer;
        // Back-to-front by ViewDepth. Always CPU-direct (a DrawIndexed per entry) — translucent
        // geometry never enters the GPU-driven cull path (it sorts per-submesh, not per group).
        vector<TranslucentDraw> Draws;
    };

    // The most runs of half-resolution draws a frame interleaves with the full-resolution ones,
    // each composited at its own place in the back-to-front order.
    inline constexpr u32 MaxHalfResSegments = 4;

    // A frame's translucent draws cut into the order they composite in: Full[0], then for each
    // segment i its half-resolution run Half[i] composited over everything before it, then
    // Full[i + 1]. HalfRect[i] is the union of Half[i]'s screen rectangles, which its composite is
    // scissored to. Filled by SegmentTranslucentDraws; the plans' Push, DrawDataSet and
    // CandidateIdBuffer are the caller's.
    struct TranslucentSegments
    {
        std::array<TranslucentDrawPlan, MaxHalfResSegments + 1> Full;
        std::array<TranslucentDrawPlan, MaxHalfResSegments> Half;
        std::array<vec4, MaxHalfResSegments> HalfRect{};
        // Half-resolution runs folded into the last segment because the frame had more than
        // MaxHalfResSegments of them; the full-resolution draws between those runs draw over them.
        u32 MergedRuns = 0;
    };
}
