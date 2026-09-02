#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/RenderLayer.h>

namespace Veng
{
    class AssetManager;
    class MaterialInstance;
    class Scene;
}

namespace Veng::Renderer
{
    class Context;
    class SceneCapture;

    /// @brief Runtime capture state a CaptureSurface materializes lazily; defined in CaptureSurface.cpp.
    struct CaptureSurfaceRuntime;

    /// @brief The surface's intended sampling model, read by the entity's material.
    ///
    /// The engine renders one environment capture from the entity's world position regardless of this
    /// value — an octahedral map of the scene seen from there. The value records how the surface's
    /// material samples that map: a reflective or refractive object gathers it by reflection direction
    /// (EnvironmentProbe), a flat mirror or monitor by its surface parameterization (PlanarReflection).
    /// It is authored data for the material, not a switch on the engine's capture path.
    enum class CaptureShape : u8
    {
        /// @brief A reflective or refractive object sampling the capture by direction (a probe).
        EnvironmentProbe,
        /// @brief A flat mirror or monitor sampling the capture across its surface (a plane).
        PlanarReflection,
    };

    /// @brief When the engine re-renders the capture.
    ///
    /// Selects whether the capture tracks the live scene every frame or freezes until a system marks
    /// it dirty. The delivered capture renders one cube face per driven frame, so a full refresh spans
    /// SceneCapture::FaceCount driven frames from the moment it is (re)dirtied.
    enum class CaptureRefresh : u8
    {
        /// @brief Re-render every frame — a live mirror or a moving probe.
        EveryFrame,
        /// @brief Render once (a full-face refresh) then idle until MarkDirty is called again.
        OnDemand,
    };

    /// @brief Which frame the capture's six faces are oriented in.
    ///
    /// The capture still renders from the entity's world position either way; this selects the
    /// orientation of the face cameras, and so the frame the resulting octahedral map is sampled in.
    enum class CaptureAlignment : u8
    {
        /// @brief Render the faces along fixed world axes — a world-oriented map sampled by world direction.
        World,
        /// @brief Render the faces in the entity's own frame — a body-fixed map sampled in the entity's frame.
        ///
        /// A probe rigidly attached to a moving body (a cockpit canopy, a car mirror, a monitor on a
        /// moving platform) sees a body-fixed interior that a world-aligned map smears across its
        /// round-robin refresh as the body turns, since a full refresh spans SceneCapture::FaceCount
        /// frames. Orienting the faces to the entity holds that interior still in the map, so the
        /// refresh latency falls only on content outside the body — distant, lower-contrast, and weakly
        /// reflected. The consuming material samples the map by a direction expressed in the entity's
        /// local frame (its reconstructed object axes), not by the world direction a world-aligned map takes.
        Entity,
    };

    /// @brief A scene entity's declaration of a render-to-texture capture the engine discovers and drives.
    ///
    /// A reflected scene component, the render-to-texture sibling of GuiSurface: where GuiSurface maps a
    /// document onto a world mesh, a CaptureSurface renders the scene into a texture from the entity's
    /// world position and binds that texture's handle onto the material of the mesh it shares an entity
    /// with (its sibling MeshRenderer). The engine discovers the component in the scene it drives, builds
    /// the owned SceneCapture from the authored config on first sight, feeds it to the capture drive-list
    /// (RegisterCapture) against the component's lifetime, and drops it — self-unregistering — when the
    /// component, its entity, or its scene goes away. So a reflective or refractive surface, a mirror, or
    /// a monitor is authored data on the entity: no app-side RegisterCapture, no per-frame game code.
    ///
    /// The capture renders the scene *around* the entity, never the entity itself: the mesh the capture
    /// feeds is excluded from its own capture (CaptureView::Exclude), in every domain the capture draws.
    /// Drawing it would compound the material's own sampled term into the next capture and — because the
    /// probe sits on or inside the surface it feeds — occlude the environment behind it. The rule is
    /// unconditional and has no authored knob.
    ///
    /// The output is an octahedral map (pre-tonemap linear HDR) the material samples by direction. Its
    /// handle is a runtime bindless slot, not a cooked asset id, so Drive rebinds it onto the sibling
    /// material's named texture slot each frame — the GuiSurface per-frame rebind. The material authors
    /// the named slot (Material::SetTextureHandle, the no-resident-asset path) and this component fills
    /// it, beside a sampler slot and the optional probe-centre and capture-frame slots (see
    /// CenterSlot and OrientationSlot).
    ///
    /// **The bound material is the sibling mesh *asset*'s, which is shared by every entity drawing that
    /// asset.** The target is the first MaterialInstance of the mesh the sibling MeshRenderer names — a
    /// cooked asset, not a per-entity copy — so two entities drawing one mesh asset resolve to one
    /// MaterialInstance and one texture slot: the last driven wins and both sample that single probe.
    /// The locality is therefore per mesh asset, not per entity. A scene wanting N independently
    /// captured surfaces gives each its own mesh asset (or its own material instance); the engine
    /// warns once per run when it sees one drive pass bind two captures onto the same MaterialInstance.
    ///
    /// The runtime resources (the SceneCapture and its sampler) are materialized on the first Drive, which
    /// needs the render context and asset manager the engine supplies; a component that never drives
    /// allocates none. Teardown is the exact inverse of the bind: the component's destruction clears
    /// the slots it filled (see Unbind), so the material stops naming a bindless slot the capture's
    /// release has handed back to the free list.
    struct CaptureSurface
    {
        /// @brief Default-constructs an unmaterialized capture (its runtime is empty until Drive).
        CaptureSurface();
        /// @brief Releases the owned capture, self-unregistering it from the drive-list, and unbinds.
        ///
        /// The release hands the capture's output slot and the runtime's own sampler slot back to the
        /// bindless free list, so the material Drive last bound is cleared first (see Unbind) rather
        /// than left naming a slot the next registration reuses. Every slot a drive took comes back,
        /// so a consumer that builds and drops surfaces over a run holds a steady count.
        ~CaptureSurface();
        /// @brief Move-constructs, transferring the runtime state.
        CaptureSurface(CaptureSurface&&) noexcept;
        /// @brief Move-assigns, transferring the runtime state.
        CaptureSurface& operator=(CaptureSurface&&) noexcept;

        CaptureSurface(const CaptureSurface&) = delete;
        CaptureSurface& operator=(const CaptureSurface&) = delete;

        /// @brief The surface's sampling model, read by the entity's material (see CaptureShape).
        CaptureShape Shape = CaptureShape::EnvironmentProbe;

        /// @brief Edge length, in pixels, of each captured cube face.
        u32 Resolution = 256;

        /// @brief When the engine re-renders the capture (see CaptureRefresh).
        CaptureRefresh Refresh = CaptureRefresh::EveryFrame;

        /// @brief Which frame the capture's faces are oriented in (see CaptureAlignment).
        CaptureAlignment Alignment = CaptureAlignment::World;

        /// @brief Whether the face renders cast shadows — both directional and punctual.
        ///
        /// Off by default, with the rest of the capture's lean battery set: a capture is usually a
        /// coarse environment term sampled by direction, where an unshadowed render is cheap and the
        /// error is invisible under a reflection's contrast.
        ///
        /// **It stops being invisible when the captured environment is an enclosed interior.** Every
        /// surface inside an enclosure is lit by the directional source as though the enclosure's own
        /// walls did not occlude it, so a cabin, a room, or the inside of any shell renders uniformly
        /// flooded — brightest exactly where it should be deepest, and with no contact darkening to
        /// give the space its shape. A probe reflecting that interior therefore shows a lit box, and
        /// the defect reads as the material's fault rather than the capture's. An interior probe wants
        /// this on; an exterior one does not.
        ///
        /// The cost is **one depth-only pass per driven frame**, not six: a capture renders one face
        /// per frame, so enabling shadows adds the shadow pass to that single face render, not to a
        /// whole refresh at once. Both shadow batteries are topology changes in the face renderer, so
        /// this is read once when the runtime materializes on the first Drive and is not live-tunable.
        bool Shadows = false;

        /// @brief Render layers the capture's faces draw (see RenderLayer).
        ///
        /// DefaultEnvironmentCaptureLayers (the default) draws every layer but RenderLayer::ViewAnchored,
        /// because a capture records the environment around its position and camera-anchored decoration —
        /// a near-field particle shell, a billboard — is not part of it: drawn into the map, it would
        /// appear in every reflection or lens sampling this capture, floating at a distance it was never
        /// at. Author AllRenderLayers to capture everything, or a narrower mask to drop more.
        u32 VisibleLayers = DefaultEnvironmentCaptureLayers;

        /// @brief Name of the sibling material's texture slot the capture output binds onto.
        ///
        /// Drive binds the capture's octahedral output handle onto the material field of this name
        /// (a TextureHandle-kind field). The default "Texture" preserves the built-in binding; author
        /// a different name to match a descriptively-named material slot.
        string TextureSlot = "Texture";

        /// @brief Name of the sibling material's sampler slot the capture sampler binds onto.
        ///
        /// Drive binds the capture's clamp sampler onto the material field of this name (a
        /// SamplerHandle-kind field). The default "Sampler" preserves the built-in binding.
        string SamplerSlot = "Sampler";

        /// @brief Name of the sibling material's slot the probe's world centre binds onto; empty is off.
        ///
        /// Named, Drive writes a vec4 onto the material's Param field of this name every frame: `xyz` is
        /// the world position the map was rendered from, and `w` is a **validity flag** — 1 once the
        /// capture's output slot exists, 0 before the first Drive materializes it and again once
        /// teardown clears the slot. Empty (the default) publishes nothing, so a material that only
        /// samples by direction declares no such field.
        ///
        /// A fragment that parallax-corrects its sample needs the centre: the map is a view from the
        /// probe while the sampling ray leaves the fragment, so the correction walks that ray, in the
        /// probe's frame, against the recorded distance the capture publishes (see DepthTextureSlot) to
        /// find where the ray meets the surface the capture saw, and re-takes the sample direction
        /// from that hit. Sampling by the bare reflection direction instead treats the probe as
        /// infinitely distant, which is wrong by the whole extent of the reflected content when that
        /// content is close. SurfaceFragmentInput carries no route to a draw's own world matrix, so
        /// without this slot a consumer reimplements the position lookup the engine's capture drive
        /// already performs.
        ///
        /// The flag is what makes a consumer's fallback branch reachable: without it a fragment cannot
        /// tell "no capture yet" from "a capture centred on the world origin", and would index the
        /// bindless texture array with an unpopulated handle slot.
        string CenterSlot;

        /// @brief Name of the sibling material's slot the capture's frame binds onto; empty is off.
        ///
        /// Named, Drive writes a vec4 onto the material's Param field of this name every frame: the
        /// rotation the faces were oriented by, as a unit quaternion packed `xyz` imaginary and `w`
        /// real (PackCaptureOrientation). It rotates a direction expressed in the capture's frame
        /// into world space, so a fragment expresses a world direction in the map's frame by
        /// rotating by its conjugate. Empty (the default) publishes nothing.
        ///
        /// **A World-aligned capture publishes the identity rotation (0, 0, 0, 1)**, because its
        /// faces are the world axes and the map's frame *is* world space — so a consumer written
        /// against this slot needs no branch on the alignment, and switching a capture between the
        /// two alignments changes only the published value.
        ///
        /// An Entity-aligned capture's map is a body-fixed environment (see CaptureAlignment), so a
        /// material sampling it by direction has to express that direction in the carrier's own
        /// frame — and SurfaceFragmentInput gives a fragment no route to that frame. Without this
        /// slot a consumer reconstructs one from whatever interpolated geometry it has, which is
        /// valid only for the single surface orientation the reconstruction was derived for.
        ///
        /// It carries no validity flag of its own: CenterSlot's `w` already reports whether the
        /// capture is bound, and a material correcting a sample declares both slots or neither.
        string OrientationSlot;

        /// @brief Name of the sibling material's texture slot the octahedral distance map binds onto;
        ///        empty is off.
        ///
        /// Named, Drive publishes — beside the radiance map — a second octahedral map in the same
        /// parameterization, holding the radial distance from the probe centre to the nearest surface
        /// in each direction (in world units), with SceneCapture::DistanceSkySentinel where a
        /// direction saw no geometry. So one OctahedralUV(direction) indexes both maps, and a fragment
        /// correcting its sample can march the recorded distance rather than intersect a hand-authored
        /// stand-in volume (see CenterSlot).
        ///
        /// **Empty (the default) renders no distance map at all** — no second resample, no second
        /// image, no bindless slots — so a capture nobody asks a distance of pays nothing. This is the
        /// opt-in, and it is what gates the work, not a flag beside it.
        ///
        /// The distance map carries no validity flag of its own: CenterSlot's `w` reports whether the
        /// capture — and thus, when this names a slot, its distance map — is bound, and teardown
        /// returns this handle to the unbound sentinel with the radiance one. A consumer that marches
        /// the distance map declares this slot together with CenterSlot; the two are not automatically
        /// in step, since a capture may name a centre and no distance map (this empty), so a consumer
        /// samples the distance map only when it, too, names a slot the capture fills.
        string DepthTextureSlot;

        /// @brief Name of the sibling material's sampler slot the distance map's sampler binds onto.
        ///
        /// A separate slot rather than the radiance map's, because the distance map wants **point
        /// filtering**: a bilinear tap across a depth discontinuity interpolates between a near surface
        /// and a far one and yields a distance at which nothing is, which reads as a smeared halo at
        /// every silhouette in the reflection. Named alongside DepthTextureSlot; Drive binds a clamp
        /// **point** sampler onto the material field of this name (a SamplerHandle-kind field).
        string DepthSamplerSlot = "DepthSampler";

        /// @brief Edge length, in pixels, of each face of the distance capture; 0 is invalid.
        ///
        /// Sizes the depth atlas cell and, at twice this, the octahedral distance map. Its own knob,
        /// defaulting to a fraction of Resolution rather than to it: distance varies far more smoothly
        /// than radiance across a captured environment, so the distance map is cheap to under-size and
        /// expensive to over-size. Read when the runtime materializes; used only when DepthTextureSlot
        /// names a slot.
        u32 DepthResolution = 128;

        /// @brief Runtime capture state, materialized on the first Drive; empty until then.
        mutable Unique<CaptureSurfaceRuntime> Runtime;

        /// @brief Requests a refresh: the next SceneCapture::FaceCount driven frames re-render the map.
        ///
        /// An OnDemand capture idles after its refresh completes; a system calls this when the sampled
        /// scene content changed so the frozen map is rebuilt. An EveryFrame capture refreshes anyway,
        /// so this is a no-op for it. Before the runtime materializes, it is remembered and applied on
        /// the first Drive.
        ///
        /// Const because the refresh state lives in the runtime record, not the authored config — the
        /// same reason Drive is const, so a read-only view of a scene can still re-arm a capture.
        void MarkDirty() const;

        /// @brief Returns the owned capture, or nullptr before the first Drive materializes it.
        [[nodiscard]] SceneCapture* GetCapture() const;

        /// @brief Returns the capture's octahedral output handle, or an invalid handle before Drive.
        ///
        /// The runtime bindless slot Drive binds onto the sibling material; the material samples the
        /// scene-from-here through it.
        [[nodiscard]] TextureHandle GetOutputHandle() const;

        /// @brief Whether the next Drive will still push a face into the capture (the refresh state).
        ///
        /// True while a refresh is in progress: always for EveryFrame, and for OnDemand until its
        /// FaceCount-frame refresh completes (re-armed by MarkDirty). False for a settled OnDemand
        /// capture — the point at which it renders nothing until dirtied again.
        [[nodiscard]] bool IsRefreshing() const;

        /// @brief Builds and drives the capture, then binds its output onto the sibling material.
        ///
        /// Materializes the runtime on first use (creating the SceneCapture at the authored resolution
        /// and its sampler) and returns the capture so the caller registers it on the drive-list the
        /// first time it appears. Pushes this frame's capture source (SceneCapture::SetView from @p
        /// position, at @p alpha) when the refresh policy calls for it — every frame for EveryFrame,
        /// only while a refresh is outstanding for OnDemand — so a settled OnDemand capture records
        /// nothing. Binds the
        /// capture's output handle onto @p material's named slot every frame (SetTextureHandle writes the
        /// current frame-in-flight region, so the handle must land regardless of whether a face was
        /// pushed), beside the sampler, — when CenterSlot names one — @p position with its validity
        /// flag, — when OrientationSlot names one — @p faceBasis as a quaternion, and — when
        /// DepthTextureSlot names one — the octahedral distance map and its point sampler. Only slots
        /// the material declares at the matching field kind are written.
        ///
        /// The pushed source excludes @p entity (CaptureView::Exclude), so the capture never draws the
        /// mesh it feeds — the rule has no authoring surface and cannot be misconfigured.
        ///
        /// The material is taken as a resident handle rather than a raw pointer because the component
        /// keeps whatever it bound resident, so its teardown can clear those slots (see Unbind) after
        /// every other owner of the material has let go.
        /// @param context   The render context the capture allocates on.
        /// @param assets    The asset manager the capture's SceneRenderer loads its shaders through.
        /// @param world     The scene being captured, pushed as this frame's source.
        /// @param entity    The entity this component belongs to; excluded from its own capture.
        /// @param position  The world-space position the capture renders from, published as the centre.
        /// @param alpha     Interpolation fraction the captured content is drawn at, in [0, 1).
        /// @param faceBasis Orthonormal basis the face cameras are oriented by — identity for a
        ///                  World-aligned capture, the entity's own draw rotation for an Entity-aligned
        ///                  one (see CaptureAlignment and CaptureView::FaceBasis). The caller resolves it
        ///                  at @p alpha alongside @p position so the faces and the content share one pose.
        /// @param material  The sibling mesh material to bind onto; an unloaded handle skips binding.
        /// @pre @p position is resolved at @p alpha — for an entity-tracking probe, through
        ///      Scene::GetInterpolatedWorldTransform at this same alpha. The two place the capture's
        ///      camera and its content on one pose; resolving the position against the
        ///      un-interpolated pose instead offsets everything rigidly attached to the capture's
        ///      carrier by a fraction of a tick's motion, which changes every frame as the alpha
        ///      sweeps (see CaptureView::Alpha).
        /// @return The owned capture (built on first use), or nullptr when the resolution is invalid.
        SceneCapture* Drive(Context& context, AssetManager& assets, const Scene& world,
                            Entity entity, const vec3& position, f32 alpha, const mat3& faceBasis,
                            const AssetHandle<MaterialInstance>& material) const;

        /// @brief Clears the material slots the last Drive filled — the exact inverse of its bind.
        ///
        /// Writes the unbound state back onto the material Drive last bound: an invalid handle into the
        /// texture and sampler slots and a zero vec4 into the centre slot, so the centre's validity flag
        /// reads 0 and a consumer takes its no-capture fallback branch. The orientation slot takes the
        /// identity rotation rather than a zero vec4 — the centre's flag is what gates the sample, so
        /// the frame is unread here, and a quaternion slot left at zero normalizes to a NaN in a
        /// consumer that reads it ungated. Only slots this component
        /// actually wrote are touched, and it forgets the binding — so it is idempotent, and a no-op
        /// before the first Drive. The destructor calls it, which is what keeps a material from sampling
        /// a bindless slot the capture's release has handed back; a consumer that re-points a
        /// MeshRenderer at a different mesh calls it to release the old material.
        ///
        /// @warning A handle slot returns to the unbound sentinel, not to any cooked default the
        ///          material authored for it — these slots are runtime-bound. A fragment reading one
        ///          without checking the centre's validity flag indexes the bindless array with it.
        void Unbind() const;
    };

    /// @brief Packs a capture's face basis into the vec4 an orientation slot carries.
    ///
    /// The basis is orthonormal, so the rotation it expresses is a unit quaternion; the vec4 holds
    /// it `xyz` imaginary and `w` real, which is the packing every consumer of
    /// CaptureSurface::OrientationSlot reads. The identity basis — what a World-aligned capture is
    /// driven with — packs to (0, 0, 0, 1). The result is normalized, so a basis carrying the
    /// rounding a normalized draw rotation leaves still yields a unit quaternion.
    ///
    /// @param faceBasis Orthonormal basis the face cameras are oriented by (see CaptureAlignment).
    /// @return The capture-frame → world rotation as a quaternion, in xyzw order.
    [[nodiscard]] VE_API vec4 PackCaptureOrientation(const mat3& faceBasis);
}

VE_ENUM(::Veng::Renderer::CaptureShape, 0x497C9B89E8057D21ULL)
VE_ENUMERATOR(EnvironmentProbe)
VE_ENUMERATOR(PlanarReflection)
VE_ENUM_END();

VE_ENUM(::Veng::Renderer::CaptureRefresh, 0x604C3FD0FE4F9840ULL)
VE_ENUMERATOR(EveryFrame)
VE_ENUMERATOR(OnDemand)
VE_ENUM_END();

VE_ENUM(::Veng::Renderer::CaptureAlignment, 0xBF0441F13ADF33AFULL)
VE_ENUMERATOR(World)
VE_ENUMERATOR(Entity)
VE_ENUM_END();

VE_REFLECT(::Veng::Renderer::CaptureSurface, 0x59B48CAC6127A406ULL)
VE_FIELD(Shape, .DisplayName = "Shape")
VE_FIELD(Resolution, .DisplayName = "Resolution", .Display = {.Min = 1})
VE_FIELD(Refresh, .DisplayName = "Refresh")
VE_FIELD(Alignment, .DisplayName = "Alignment")
VE_FIELD(Shadows, .DisplayName = "Shadows")
VE_FIELD(VisibleLayers, .DisplayName = "Visible Layers")
VE_FIELD(TextureSlot, .DisplayName = "Texture Slot")
VE_FIELD(SamplerSlot, .DisplayName = "Sampler Slot")
VE_FIELD(CenterSlot, .DisplayName = "Center Slot")
VE_FIELD(OrientationSlot, .DisplayName = "Orientation Slot")
VE_FIELD(DepthTextureSlot, .DisplayName = "Depth Texture Slot")
VE_FIELD(DepthSamplerSlot, .DisplayName = "Depth Sampler Slot")
VE_FIELD(DepthResolution, .DisplayName = "Depth Resolution", .Display = {.Min = 1})
VE_REFLECT_END();
