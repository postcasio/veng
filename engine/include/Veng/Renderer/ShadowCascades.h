#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Scene/Camera.h>

/// @brief Pure, device-free cascaded shadow map math.
///
/// Turns a camera, a light direction, and the scene bound into per-cascade
/// light-space view-proj matrices and split distances. glm-only — no Context, no
/// device — so it compiles and unit-tests without an ICD, beside PunctualShadows.h.
///
/// A cascade holds still against the scene: its radius rounds up onto a fixed ladder of sizes,
/// so its texel pitch changes only in whole steps, and its box is snapped to that pitch on a grid
/// fixed in the frame the scene's content rests in (CascadeFrame), so a camera that translates or
/// turns moves the box by whole texels and a point at rest keeps its sub-texel position.
namespace Veng::Renderer
{
    /// @brief Maximum number of shadow cascades the engine supports.
    inline constexpr u32 MaxCascades = 4;

    /// @brief Maximum number of independent cascade sets the directional shadow atlas carries.
    ///
    /// A scene lit by several near-parallel sources shadows from more than one of them: each
    /// granted source gets its own set of MaxCascades cascades, fit to its own direction, stacked
    /// as extra tile rows in the one atlas. Two, not more, because a set is the atlas's expensive
    /// unit: at the default 1024² tile and four cascades one set is a 2048² D32 atlas (16 MiB) and
    /// each further set adds another 16 MiB plus a full re-render of every caster through four more
    /// cascade viewports — where a punctual tile costs a sixth of that. Two covers a scene with two
    /// comparable distant sources, which is the case the single-cascade rule could not express at
    /// all; a third comparable source is rare enough not to be worth 48 MiB of atlas on every scene
    /// that has one distant source. A source past the limit is denied the atlas and shades
    /// unshadowed rather than borrowing another source's cascade.
    inline constexpr u32 MaxCascadeSets = 2;

    /// @brief Output of ComputeCascades: per-cascade matrices and split distances.
    struct CascadeData
    {
        /// @brief Per-cascade world → light-clip matrices; only [0, Count) are valid.
        std::array<mat4, MaxCascades> ViewProj;

        /// @brief Per-cascade world → light-clip matrices for caster culling; [0, Count) valid.
        ///
        /// The light-axis near plane is extended toward the light (by the scene bound, or the
        /// fixed pullback when it is empty) so an off-screen caster between the light and the
        /// slice survives the cull. With PancakeNear the render ViewProj keeps a tight near and
        /// depth clamp flattens those casters onto it; without PancakeNear the two matrices are
        /// identical.
        std::array<mat4, MaxCascades> CullViewProj;

        /// @brief Each cascade's far distance in view space.
        ///
        /// The lighting pass selects a cascade by comparing the fragment's view depth
        /// against these. Element i is cascade i's far split; [0, Count) valid. With
        /// MaxCascades == 4 this array packs directly into the ShadowConstants
        /// CascadeSplits vec4.
        std::array<f32, MaxCascades> SplitFar;

        /// @brief Each cascade's world units per shadow-map texel; [0, Count) valid.
        ///
        /// The raster pitch: the box is exactly Resolution of these wide, and it is also the
        /// increment the box is snapped to. The lighting pass scales its normal-offset and depth
        /// bias by it, so bias grows with cascade coarseness instead of being recovered
        /// approximately from the matrix. It changes only when RadiusStep does.
        std::array<f32, MaxCascades> TexelWorldSize;

        /// @brief Each cascade's step on the radius ladder; [0, Count) valid.
        ///
        /// The cascade's box covers a sphere of CascadeStepRadius(RadiusStep[k]), the smallest
        /// ladder radius at or above the slice's own bounding radius, held while the need stays
        /// within one step below it (see CascadeFrame::Previous).
        std::array<i32, MaxCascades> RadiusStep;

        /// @brief Each cascade's render ortho depth extent (far - near) in world units.
        ///
        /// Converts a world-space depth bias into the NDC-z units the shadow compare runs
        /// in: one NDC-z unit spans this many world units. [0, Count) valid.
        std::array<f32, MaxCascades> DepthRange;

        /// @brief Number of valid cascades; clamp(settings.Count, 1, MaxCascades).
        u32 Count = 0;

        /// @brief How many leading cascades snap in scene space rather than the stable frame.
        ///
        /// Cascade k takes the viewer's frame when its slice begins nearer than
        /// CascadeFrame::ViewerFrameReach; the splits ascend, so those cascades are always the
        /// prefix [0, ViewerFrameCount). Recomputed from each fit's splits.
        u32 ViewerFrameCount = 0;
    };

    /// @brief Ladder steps per doubling of a cascade's radius.
    inline constexpr i32 CascadeRadiusStepsPerOctave = 8;

    /// @brief The radius, in world units, of a step on the cascade radius ladder.
    ///
    /// Step n is 2^(n / CascadeRadiusStepsPerOctave) world units; n may be negative.
    /// @param step  The ladder step.
    /// @return The step's radius.
    [[nodiscard]] f32 CascadeStepRadius(i32 step);

    /// @brief The light-space basis a directional light's cascades are snapped in.
    ///
    /// The rows are the light-space right, up and back axes of a light travelling along
    /// @p lightDir, glm::lookAt's, taking world +Y as up unless the light points within about 8°
    /// of straight up or down, where +Z is. It depends on the light alone, so a cascade's grid
    /// turns only when its light does: a point p lies on the grid at basis · p.
    /// @param lightDir  World-space direction the light travels; need not be normalized.
    /// @return The rotation from world (or stable-frame) axes to light-space axes.
    [[nodiscard]] mat3 CascadeLightBasis(vec3 lightDir);

    /// @brief The frame a cascade fit snaps its grid in, and the history its ladder holds against.
    ///
    /// A fit snaps each cascade's box to whole texels of a grid fixed in the frame the scene's
    /// content rests in. For a scene whose coordinate origin stays put that frame is scene space,
    /// and the defaults describe it. A consumer that re-centres its world on the viewer each frame
    /// passes where its origin stands in the stable frame (Origin), so the grid stays put while
    /// scene space moves under it, and how far out its viewer carries geometry of its own
    /// (ViewerFrameReach), whose cascades snap in scene space instead.
    struct CascadeFrame
    {
        /// @brief The scene's coordinate origin, placed in the stable frame.
        ///
        /// A point at scene coordinates p lies at p + Origin in the stable frame. A translation
        /// only. Read at double precision; only its phase against each cascade's pitch matters.
        dvec3 Origin{0.0};

        /// @brief View depth out to which the viewer carries geometry of its own.
        ///
        /// Every cascade whose slice begins nearer than this snaps in scene space, which rides
        /// the viewer; the rest snap in the stable frame. Zero selects no cascade.
        f32 ViewerFrameReach = 0.0f;

        /// @brief Last frame's fit of this cascade set, for the ladder's hysteresis; may be null.
        ///
        /// With it, each cascade keeps its previous step while the need sits at most one step
        /// below it, and takes the need's step otherwise, so the size grows at once and does not
        /// flip on a need hovering across a step boundary. Ignored when its Count differs.
        const CascadeData* Previous = nullptr;
    };

    /// @brief Inputs that control how cascades are split and sized.
    struct CascadeSettings
    {
        /// @brief Requested cascade count; clamped to [1, MaxCascades].
        u32 Count = 4;
        /// @brief PSSM split blend: 0 = uniform splits, 1 = logarithmic.
        f32 Lambda = 0.85f;
        /// @brief Per-cascade tile edge in texels; drives texel snapping. Must be even and above 4.
        u32 Resolution = 1024;
        /// @brief View-space cap on the shadowed range; 0 disables the cap.
        ///
        /// The fitted far split is clamped to this distance, so a distant camera far plane
        /// (or a scene larger than shadows usefully cover) cannot spread the cascades thin.
        /// The lighting pass fades shadows out approaching the last split, so the cap reads
        /// as a fade, not a hard edge.
        f32 MaxDistance = 0.0f;
        /// @brief View-space floor on the shadowed range's near plane; 0 uses the camera near.
        ///
        /// The split distribution and the per-cascade slice reconstruction run over
        /// [near, far], and both degenerate when the camera near is tiny against a distant
        /// far: the logarithmic splits pack every near cascade into a sub-pixel shell at the
        /// eye, and the camera-range slice fractions underflow together and collapse the
        /// reconstructed corners. A consumer rendering a huge depth range under reverse-Z
        /// (where a near of microns keeps full far-field depth precision) hits exactly that.
        /// Flooring the shadow near here bounds the fit independently of the render near —
        /// the near-side mirror of MaxDistance — so the cascades fit the receivers that
        /// matter rather than collapsing. The floor never rises above the fitted far, so a
        /// scene whose casters all sit nearer than it keeps a valid range.
        f32 MinDistance = 0.0f;
        /// @brief Keep each cascade's ortho near tight to its slice for depth-clamped rendering.
        ///
        /// Requires the shadow pipeline to rasterize with depth clamp enabled: casters between
        /// the light and the slice fall outside the tight near plane and are pancaked onto it
        /// instead of clipped. Culling uses the extended CullViewProj either way. Keeping the
        /// depth range tight makes NDC-z bias resolution independent of the scene's extent
        /// along the light.
        bool PancakeNear = false;
    };

    /// @brief Computes per-cascade fit-to-frustum light matrices.
    ///
    /// lightDir is the light's travel direction. sceneBounds (world space, possibly
    /// empty) does two things: it clamps the split range to the view-depth extent of the
    /// camera-frustum ∩ scene-bound intersection, so the cascades fit only the visible
    /// receivers rather than the camera's full clip range or the scene's full depth, and
    /// it extends each cascade's cull matrix's near plane toward the light so off-screen
    /// casters are included (the render matrix too, unless settings.PancakeNear keeps it
    /// tight for a depth-clamped shadow pass). settings.MaxDistance additionally caps the
    /// fitted far split.
    ///
    /// Each slice is bounded by a sphere measured in view space, so for given splits its size
    /// does not depend on the camera's orientation. The radius rounds up onto the ladder
    /// (CascadeStepRadius), the texel pitch is 2·radius / (Resolution − 4), and the box is exactly
    /// Resolution texels of that pitch, centred on the sphere snapped to the pitch in the frame
    /// @p frame names — so the sphere keeps at least one texel to spare on each side, and a box
    /// move is a whole number of texels. The light basis depends on the light alone.
    /// @param camera       The view camera that defines the split frustum.
    /// @param lightDir     World-space direction the light travels (toward receivers).
    /// @param sceneBounds  World-space scene bound; may be empty.
    /// @param settings     Split count, lambda, and tile resolution.
    /// @param frame        The snap frame and last frame's fit; the default is a fixed-origin
    ///                     scene with no history.
    /// @return Per-cascade matrices and view-space split distances.
    /// @pre settings.Resolution is even and greater than 4.
    [[nodiscard]] CascadeData ComputeCascades(const CameraView& camera, vec3 lightDir,
                                              const AABB& sceneBounds,
                                              const CascadeSettings& settings,
                                              const CascadeFrame& frame = {});

    /// @brief Tile layout of the directional shadow atlas for a cascade count and set count.
    ///
    /// One set occupies min(Count,2) columns × ceil(Count/2) rows of square tiles: 1×1 for one
    /// cascade, 2×1 for two, 2×2 for three or four. A low cascade count pays for no idle tiles.
    /// Cascade k of set s maps to tile (k % Columns, s · Rows + k / Columns) — sets stack as
    /// further row bands, so the atlas is Columns wide and TotalRows() tall however many sets it
    /// carries. Both the render pass (per-cascade viewport) and the lighting-constant tile remap
    /// derive their layout from this.
    struct ShadowAtlasGrid
    {
        /// @brief Number of tile columns in the atlas.
        u32 Columns;
        /// @brief Number of tile rows one cascade set occupies.
        u32 Rows;
        /// @brief Number of stacked cascade sets.
        u32 Sets;

        /// @brief Total tile rows in the atlas: one set's rows times the set count.
        [[nodiscard]] u32 TotalRows() const { return Rows * Sets; }
    };

    /// @brief Returns the shadow atlas tile grid for the given cascade count and set count.
    /// @param cascadeCount  Requested cascade count; clamped to [1, MaxCascades].
    /// @param setCount      Requested cascade-set count; clamped to [1, MaxCascadeSets].
    [[nodiscard]] inline ShadowAtlasGrid ComputeShadowAtlasGrid(u32 cascadeCount, u32 setCount)
    {
        const u32 count =
            cascadeCount < 1 ? 1 : (cascadeCount > MaxCascades ? MaxCascades : cascadeCount);
        const u32 sets = setCount < 1 ? 1 : (setCount > MaxCascadeSets ? MaxCascadeSets : setCount);
        const u32 columns = count < 2 ? count : 2;
        const u32 rows = (count + 1) / 2;
        return {.Columns = columns, .Rows = rows, .Sets = sets};
    }

    /// @brief Bakes an atlas-tile remap into a cascade's world → light-clip matrix.
    ///
    /// A fragment projected by the result lands in the tile cascade @p cascade of set @p set
    /// occupies, so the lighting pass samples the correct tile by construction. The transform maps
    /// NDC.xy in [-1,1] → the tile's window and back to the [-1,1] clip the sample's
    /// `NDC.xy * 0.5 + 0.5` undoes; Z is left unchanged (the depth compare is tile-agnostic).
    /// Cascade k of set s maps to tile (k % Columns, s · Rows + k / Columns), matching
    /// ComputeShadowAtlasGrid's layout.
    /// @param cascadeViewProj  The cascade's world → light-clip matrix.
    /// @param cascade          Cascade index within the set, used to select the tile.
    /// @param set              Cascade-set index, selecting the atlas row band.
    /// @param grid             Atlas tile grid the tile is placed in.
    /// @return The tile-remapped world → light-clip matrix.
    [[nodiscard]] mat4 ComposeTileRemap(const mat4& cascadeViewProj, u32 cascade, u32 set,
                                        const ShadowAtlasGrid& grid);
}
