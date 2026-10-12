#pragma once

#include <array>
#include <span>

#include <Veng/Veng.h>
#include <Veng/Asset/MeshLod.h>
#include <Veng/Math/AABB.h>
#include <Veng/Scene/Entity.h>

namespace Veng::Renderer
{
    /// @brief One copy of an instanced mesh set's mesh: its transform and its custom datum.
    ///
    /// The transform carries the mesh's object space into the set's frame (the InstancedMesh
    /// entity's world transform) as the three rows of a 3x4 affine matrix: rotation and scale in
    /// xyz, translation in w. The datum is the consumer's — a colour, a seed, a variant — and a
    /// material reads it through Veng/instanced_mesh.slang's InstanceDatum. Laid out as the shader's
    /// InstanceRecord, 64 bytes.
    struct MeshInstance
    {
        /// @brief The transform's first row: x's coefficients, then x's translation.
        vec4 Row0{1.0f, 0.0f, 0.0f, 0.0f};
        /// @brief The transform's second row.
        vec4 Row1{0.0f, 1.0f, 0.0f, 0.0f};
        /// @brief The transform's third row.
        vec4 Row2{0.0f, 0.0f, 1.0f, 0.0f};
        /// @brief The consumer's per-instance data.
        vec4 Datum{0.0f};

        /// @brief Builds an instance from a position, a rotation and a uniform scale.
        /// @param position Where the instance's origin stands in the set's frame.
        /// @param rotation The instance's orientation in the set's frame.
        /// @param scale    The instance's uniform scale.
        /// @param datum    The consumer's per-instance data.
        /// @return The instance.
        [[nodiscard]] static MeshInstance Make(const vec3& position, const quat& rotation,
                                               f32 scale, const vec4& datum = vec4(0.0f));

        /// @brief Builds an instance from an affine matrix (its last row is ignored).
        /// @param transform The object -> set-frame transform.
        /// @param datum     The consumer's per-instance data.
        /// @return The instance.
        [[nodiscard]] static MeshInstance FromMatrix(const mat4& transform,
                                                     const vec4& datum = vec4(0.0f));

        /// @brief Returns the transform as a 4x4 matrix.
        [[nodiscard]] mat4 GetMatrix() const;

        /// @brief Returns the instance's position (the transform's translation).
        [[nodiscard]] vec3 GetPosition() const { return {Row0.w, Row1.w, Row2.w}; }
    };

    static_assert(sizeof(MeshInstance) == 64, "MeshInstance must match the shader InstanceRecord");

    /// @brief The instances of one instanced mesh set: a CPU store the renderer uploads.
    ///
    /// A store has a capacity (the instances it holds) and a count (the prefix of them drawn).
    /// Writing records the range it touched, and each renderer drawing the set uploads only the
    /// ranges written since it last uploaded into the copy a frame is about to read, so a frame
    /// never waits on another still in flight. The store also keeps a conservative bound of the
    /// instances it holds, from which the renderer bounds the whole set without visiting them.
    ///
    /// Render-thread only, like the rest of the renderer.
    class InstanceSet
    {
    public:
        /// @brief The most write ranges kept for uploads; a renderer further behind uploads all.
        static constexpr u32 MaxTrackedWrites = 32;

        /// @brief A half-open instance range [First, End).
        struct Range
        {
            /// @brief The range's first instance.
            u32 First = 0;
            /// @brief One past its last instance.
            u32 End = 0;
        };

        /// @brief Constructs an empty store; Create is the usual way to make one.
        InstanceSet();

        /// @brief Creates a store holding @p capacity identity instances, all drawn.
        /// @param capacity The instances the store holds.
        /// @return The store.
        [[nodiscard]] static Ref<InstanceSet> Create(u32 capacity = 0);

        /// @brief Changes the capacity, keeping the instances that fit and default-filling new ones.
        ///
        /// The count clamps to the new capacity. Resizing reallocates every renderer's copy of the
        /// set, so it is the expensive change; a varying population is a count change.
        /// @param capacity The new capacity.
        void Resize(u32 capacity);

        /// @brief Sets how many instances, from the first, are drawn.
        /// @param count The drawn count, at most the capacity.
        void SetCount(u32 count);

        /// @brief Overwrites instances [first, first + instances.size()).
        /// @param first     The first instance written.
        /// @param instances The new instances.
        /// @pre first + instances.size() <= GetCapacity().
        void Write(u32 first, std::span<const MeshInstance> instances);

        /// @brief Tightens the bound to exactly the drawn instances.
        ///
        /// Writing only ever grows the bound, so a set whose instances moved inward keeps a looser
        /// one than it needs until this runs. It visits every drawn instance.
        void RecomputeBounds();

        /// @brief Returns how many instances are drawn.
        [[nodiscard]] u32 GetCount() const { return m_Count; }

        /// @brief Returns how many instances the store holds.
        [[nodiscard]] u32 GetCapacity() const { return static_cast<u32>(m_Instances.size()); }

        /// @brief Returns every instance the store holds, drawn or not.
        [[nodiscard]] std::span<const MeshInstance> GetInstances() const { return m_Instances; }

        /// @brief Returns a bound, in the set's frame, of a mesh drawn at every drawn instance.
        ///
        /// Conservative: it holds each instance's mesh bounding sphere, measured as the renderer's
        /// level-of-detail rule measures it, and may be larger than they need until
        /// RecomputeBounds runs. Empty when nothing is drawn.
        /// @param meshBounds The mesh's level-0 bound, in its object space.
        /// @return The set-frame bound.
        [[nodiscard]] AABB GetLocalBounds(const AABB& meshBounds) const;

        /// @brief Returns a value no other store in the process has carried.
        [[nodiscard]] u64 GetId() const { return m_Id; }

        /// @brief Returns a counter that moves each time the capacity changes.
        [[nodiscard]] u64 GetLayoutSerial() const { return m_LayoutSerial; }

        /// @brief Returns a counter that moves on every write and every capacity change.
        [[nodiscard]] u64 GetWriteSerial() const { return m_WriteSerial; }

        /// @brief Returns the instances written after the write serial @p serial was current.
        ///
        /// The union of every range written since, or the whole store when @p serial predates the
        /// ranges kept or the last capacity change. Empty when nothing was written since.
        /// @param serial A value GetWriteSerial returned earlier.
        /// @return The range to upload.
        [[nodiscard]] Range GetWrittenSince(u64 serial) const;

    private:
        /// @brief One recorded write.
        struct WriteRecord
        {
            /// @brief The write serial the write produced.
            u64 Serial = 0;
            /// @brief The range written.
            Range Written;
        };

        /// @brief Grows the bound to hold one instance.
        void GrowBounds(const MeshInstance& instance);

        /// @brief The store's process-unique id.
        u64 m_Id = 0;
        /// @brief Every instance held.
        vector<MeshInstance> m_Instances;
        /// @brief How many are drawn.
        u32 m_Count = 0;
        /// @brief The bound of the held instances' origins.
        AABB m_OriginBounds = AABB::Empty();
        /// @brief The largest axis scale of any held instance's transform.
        f32 m_MaxScale = 0.0f;
        /// @brief Moves on every capacity change.
        u64 m_LayoutSerial = 1;
        /// @brief Moves on every write and capacity change.
        u64 m_WriteSerial = 1;
        /// @brief The write serial at the last capacity change; anything older uploads all.
        u64 m_WholeSerial = 1;
        /// @brief The most recent writes, oldest first.
        vector<WriteRecord> m_Writes;
    };

    /// @brief The largest axis scale of a model matrix's upper 3x3.
    /// @param model The matrix.
    /// @return The length of its longest basis column.
    [[nodiscard]] f32 MaxAxisScale(const mat4& model);

    /// @brief The bounding sphere the level-of-detail rule measures a mesh by under a model matrix.
    ///
    /// Its centre is the mesh's level-0 bound's centre through the matrix, its radius the bound's
    /// half-diagonal times the matrix's largest axis scale. The C++ twin of
    /// Veng/instanced_mesh_math.slang's InstanceBoundingSphere.
    /// @param model      The object -> world matrix.
    /// @param meshBounds The mesh's level-0 bound.
    /// @param center     Receives the world-space centre.
    /// @param radius     Receives the world-space radius.
    void InstanceBoundingSphere(const mat4& model, const AABB& meshBounds, vec3& center,
                                f32& radius);

    /// @brief The size in pixels a sphere projects to in an instanced set's reference view.
    ///
    /// The size an InstancedMesh's MinProjectedPixels / MaxProjectedPixels band is measured by, in
    /// every view: the sphere's projected diameter as a fraction of the view's height — the
    /// level-of-detail rule's size, with no bias — times the view's height in pixels. The renderer's
    /// reference view is the main camera: its unjittered view-projection, its position and its
    /// rendered height (the shader's ViewConstants CurViewProj, CameraPosition and ExtentParams.y).
    /// The C++ twin of Veng/instanced_mesh_math.slang's ReferenceProjectedPixels.
    /// @param referenceViewProj     The reference view's world -> clip matrix.
    /// @param referenceEye          The reference view's eye.
    /// @param referenceHeightPixels The reference view's height in pixels.
    /// @param center                The sphere's world-space centre.
    /// @param radius                The sphere's world-space radius.
    /// @return The size in pixels; the largest f32 for a sphere containing the eye.
    [[nodiscard]] f32 ReferenceProjectedPixels(const mat4& referenceViewProj,
                                               const vec3& referenceEye, f32 referenceHeightPixels,
                                               const vec3& center, f32 radius);

    /// @brief What one view kept of one instanced mesh set in the last measured frame.
    struct InstancedMeshViewStats
    {
        /// @brief The set's entity.
        Entity Set;
        /// @brief The view: 0 the main camera, then the frame's shadow views in render order.
        u32 View = 0;
        /// @brief The survivors per level of detail.
        std::array<u32, MaxMeshLods> Survivors{};
    };

    /// @brief The instanced mesh sets one renderer drew, and what each view kept of them.
    ///
    /// The set, instance and draw counts are the last Execute's. The survivors are counted on the
    /// GPU, so they are read back as many frames later as there are frames in flight, and cover the frame that
    /// wrote them.
    struct InstancedMeshStats
    {
        /// @brief Sets drawn in the last Execute.
        u32 Sets = 0;
        /// @brief Instances those sets carry.
        u32 Instances = 0;
        /// @brief Views their instances were culled for: the main camera and each shadow view.
        u32 Views = 0;
        /// @brief (view, set) pairs culled per instance; a view rejecting a set whole culls none.
        u32 Jobs = 0;
        /// @brief Indirect or direct draws the sets' passes issue (one per level and submesh).
        u32 Draws = 0;
        /// @brief Whether the cull ran on the GPU; false on the CPU fallback.
        bool GpuCulled = false;
        /// @brief Per (set, view), the survivors per level, from the measured frame.
        vector<InstancedMeshViewStats> PerView;
    };

    /// @brief One survivor of an instanced mesh set's cull in one view.
    struct InstancedMeshSurvivor
    {
        /// @brief The instance's index in its set.
        u32 Instance = 0;
        /// @brief The level of detail it drew at.
        u32 Lod = 0;
    };

    /// @brief Every survivor of one instanced mesh set in one view of the last Execute.
    struct InstancedMeshSurvivors
    {
        /// @brief The set's entity.
        Entity Set;
        /// @brief The view: 0 the main camera, then the frame's shadow views in render order.
        u32 View = 0;
        /// @brief The survivors, level by level, unordered within a level.
        vector<InstancedMeshSurvivor> Survivors;
    };
}
