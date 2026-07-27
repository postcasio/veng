#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Mesh.h> // MeshData, CanonicalVertex

namespace Veng
{
    class MaterialInstance;
}

/// @brief Runtime primitive mesh generators.
///
/// Each function returns CPU-side MeshData in the canonical vertex layout
/// (Mesh::CanonicalLayout()); upload with Mesh::BuildSync(context, data, name).
/// Geometry is generated with analytic normals, tangents (xyz + handedness w),
/// and UVs. A valid material handle is recorded on the produced submesh
/// (the mesh owns it; the draw loop binds it); an empty handle leaves the
/// submesh unassigned (SubMesh::NoMaterial).
namespace Veng::Primitives
{
    /// @brief Axis-aligned cube centered at the origin, `extent` units across the full width (±extent/2 per axis).
    ///
    /// 24 vertices (4 per face, hard normals), 36 indices, one submesh. Per-face UVs span [0,1].
    [[nodiscard]] MeshData Cube(f32 extent = 1.0f, AssetHandle<MaterialInstance> material = {});

    /// @brief Flat XZ-plane quad (+Y normal) centered at the origin, tessellated into `subdivisions` quads per axis.
    ///
    /// UVs span [0,1] across the plane. Minimum 1 subdivision per axis.
    [[nodiscard]] MeshData Plane(vec2 size = vec2(1.0f), uvec2 subdivisions = uvec2(1),
                                 AssetHandle<MaterialInstance> material = {});

    /// @brief Perspective-true display shell: the spherical-cap section that reproduces a screen rect of a perspective projection.
    ///
    /// Geometry generated *from* a camera projection. A regular `subdivisions` grid is laid over the
    /// normalized screen rect (`rectCenter` / `rectSize`, in `[0,1]` window fractions), each grid
    /// point is unprojected through the perspective (`fovY`, `aspect`) into a camera-space ray
    /// (−Z forward, +Y up — the CameraView convention), and its vertex is placed at `radius` along
    /// that ray. Every vertex is therefore the same distance from the local origin — a collimated,
    /// equidistant display, unlike a flat quad whose corners sit further from the eye than its centre.
    ///
    /// The contract: **rendered through a perspective projection of the same `fovY` and `aspect` from
    /// the local origin looking down −Z, the shell reproduces the screen rect exactly at its
    /// vertices, and between them to within ProjectionShellReprojectionBound(); from any other pose
    /// it is ordinary geometry.** Off the reference pose it skews and parallaxes like the physical
    /// pane it is, which is the point — a cockpit head-up display on glass, a helmet visor readout,
    /// a collimated instrument all want a display that agrees with a screen-space layout from one
    /// eye and behaves like an object from every other.
    ///
    /// **Camera space is +Y up, but the rect fractions and the UVs are top-left origin, y-down.**
    /// Both conventions are load-bearing and they differ deliberately. A rect fraction of (0,0) is
    /// the window's *top-left* and maps to camera-space +Y, because the engine projection bakes the
    /// Vulkan Y flip so a top-left fraction maps to NDC directly with no second flip (the same fact
    /// ScreenToWorldRay in Veng/Scene/SceneSystem.h relies on). UV.y runs the same way — 0 at the
    /// rect's top edge — so the shell samples a document in the orientation the document lays out in.
    /// @warning A generator written to "+Y up" without stating which way V runs produces a
    ///          vertically mirrored document, and a test that reprojects a vertex through the
    ///          generator's own inverse passes anyway. Check the mapping with ProjectToScreen
    ///          (Veng/Scene/Camera.h), which is the projection the renderer actually uses.
    ///
    /// Normals point back toward the local origin (−ray), tangents follow +U, the winding faces the
    /// origin, and UVs are the grid parameter across the rect. One submesh.
    /// @param fovY          Vertical field of view in radians of the projection the shell is derived from.
    /// @param aspect        Viewport width divided by height of that same projection.
    /// @param rectCenter    Centre of the covered screen rect, in [0,1] window fractions (top-left origin).
    /// @param rectSize      Size of the covered screen rect, in window fractions.
    /// @param radius        Distance from the local origin to every vertex, in world units.
    /// @param subdivisions  Grid cells per axis across the rect; clamped to at least 1 each.
    /// @param material      Material recorded on the produced submesh; empty leaves it unassigned.
    /// @pre `radius > 0`, `aspect > 0`, `0 < fovY < pi`, and both components of `rectSize` are > 0.
    /// @see ProjectionShellReprojectionBound
    [[nodiscard]] MeshData ProjectionShell(f32 fovY, f32 aspect, vec2 rectCenter, vec2 rectSize,
                                           f32 radius, uvec2 subdivisions,
                                           AssetHandle<MaterialInstance> material = {});

    /// @brief Worst-case reprojection displacement of a ProjectionShell at its reference pose, in logical points.
    ///
    /// A shell is exact at its vertices by construction; between them the rasterizer's
    /// perspective-correct interpolation runs along the flat chord rather than the ideal spherical
    /// arc, so a document feature lands slightly off where the screen-space layout puts it. This is
    /// the closed form of that displacement — the number an alignment budget is checked against and
    /// the threshold a comparison against a screen-space layout uses, rather than a tuned constant.
    /// It is independent of the shell's `radius`: scaling every vertex about the eye leaves the
    /// projection unchanged.
    ///
    /// The derivation, in one pass. Write a grid point's screen fraction as `f` and its unprojected
    /// direction at unit forward depth as `D(f) = ((2f.x−1)·aspect·T, −(2f.y−1)·T, −1)` with
    /// `T = tan(fovY/2)`, so its vertex is `radius·D/|D|`. Two vertices' chord is
    /// `C(s) = (1−s)·P0 + s·P1`; since `P_i = λ_i·D_i` with `λ_i = radius/|D_i|` and every `D_i` has
    /// `z = −1`, the chord projects onto the *straight* screen segment between `f0` and `f1` — the
    /// error is a pure reparametrization along that segment, never a bow away from it. Solving for it
    /// gives `s(1−s)·|r1−r0| / ((1−s)·r1 + s·r0)` of the segment's length, with `r_i = |D_i|`, whose
    /// maximum sits at the midpoint and is `|r1−r0|·|Δ| / (4·r)` for a segment of screen length `|Δ|`.
    ///
    /// With `r(a) = sqrt(1 + (aspect·T·a.x)² + (T·a.y)²)` over `a = 2f−1`, the gradient is
    /// `∂r/∂f.x = 2(aspect·T)²·a.x/r` and `∂r/∂f.y = 2T²·a.y/r`; a cell spans
    /// `φ = rectSize/subdivisions` in fractions and `Δ = φ·windowExtent` in logical points, and its
    /// worst chord is the diagonal (whose two gradient terms add, in the quadrant where their signs
    /// agree — which the symmetric rect always contains, so which diagonal the triangulation uses
    /// does not matter). Hence the bound
    ///
    ///     max over the rect of  ((aspect·T)²·|a.x|·φ.x + T²·|a.y|·φ.y) / r(a)²  ·  |Δ| / 2
    ///
    /// maximized over `|a.x| ≤ |2·rectCenter.x−1| + rectSize.x` and likewise in y — a bounded
    /// two-variable rational maximisation this evaluates exactly, at its interior stationary point or
    /// on the box boundary. The `Δ²` in the product makes the error `O(cell²)`, so it falls
    /// quadratically as the grid is refined.
    /// @param fovY          Vertical field of view in radians of the projection the shell was built from.
    /// @param aspect        Viewport width divided by height of that projection.
    /// @param rectCenter    Centre of the covered screen rect, in window fractions.
    /// @param rectSize      Size of the covered screen rect, in window fractions.
    /// @param subdivisions  Grid cells per axis; clamped to at least 1 each, as the generator clamps them.
    /// @param windowExtent  Window extent, in logical points, the returned displacement is measured in.
    /// @return The largest displacement, in logical points, between a document feature's ideal screen
    ///         position and where the shell places it, viewed from the reference pose.
    /// @pre `aspect > 0`, `0 < fovY < pi`, and both components of `rectSize` and `windowExtent` are > 0.
    /// @see ProjectionShell
    [[nodiscard]] f32 ProjectionShellReprojectionBound(f32 fovY, f32 aspect, vec2 rectCenter,
                                                       vec2 rectSize, uvec2 subdivisions,
                                                       vec2 windowExtent);

    /// @brief Cylindrical display panel: a rectangular sheet bent about its local +Y axis and flat along it.
    ///
    /// The geometry of a curved monitor, a curved instrument fascia, a bent signage board — a section
    /// of a cylinder's lateral surface, curved on one axis only. `size` is the panel's own physical
    /// extent: `size.x` is the **arc length** across the curve — the quantity that stays fixed as the
    /// curvature changes and the one a panel's spec sheet states, not the chord width — and `size.y`
    /// the straight height.
    ///
    /// The frame is stated rather than left to be inferred, because a sign error in it is invisible.
    /// The local origin is the panel's centre point *on its surface*; the surface normal there is
    /// **+Z**; the **centre of curvature is at local `(0, 0, +curvatureRadius)`** — on the normal's
    /// side, so the flanks bend *toward* a viewer out on +Z. That is the curved-monitor direction and
    /// the opposite of a dish. Normals point outward at that viewer, tangents follow +U, the winding
    /// faces +Z, one submesh.
    ///
    /// **UVs are top-left origin, y-down**: `(0,0)` is the panel's top-left corner seen from +Z and V
    /// increases downward — the same convention ProjectionShell documents. It is not a free choice,
    /// it is what lets a panel sample a document in the orientation the document lays out in.
    /// @warning A generator written to "+Y up" without stating which way V runs produces a
    ///          vertically mirrored document, and a test that reprojects a vertex through the
    ///          generator's own inverse passes anyway. Check the mapping with ProjectToScreen
    ///          (Veng/Scene/Camera.h), which is the projection the renderer actually uses.
    ///
    /// **Vertices are spaced uniformly in arc length**, not uniformly in the angle they subtend at any
    /// eye. That is the physical property — a panel's pixels are evenly spaced *on the glass* — and it
    /// is a substantive part of what makes the result read as a panel rather than as a screen-space
    /// overlay, so it is contract rather than an implementation detail a reader may assume either way.
    ///
    /// What density buys here differs from a ProjectionShell's: a shell's grid buys *alignment*
    /// against a closed-form bound, while a panel's buys **silhouette and shading smoothness** — the
    /// faceting visible along the curve and the granularity of the normal's sweep across it. The `y`
    /// count is nearly free: the panel is straight along +Y, so a single band is geometrically exact
    /// and further bands exist only to give a fragment shader vertex data to interpolate.
    ///
    /// A large `curvatureRadius` degenerates smoothly toward Plane's flat geometry — the deviation
    /// from flat is the sagitta `2·curvatureRadius·sin²(size.x/(4·curvatureRadius))`, which falls as
    /// `size.x²/(8·curvatureRadius)` — and nothing divides by a vanishing quantity on the way. There
    /// is no sentinel value meaning flat: `curvatureRadius > 0` is required, and Plane is the flat
    /// shape. An arc length past `2·pi·curvatureRadius` wraps the cylinder and the sheet overlaps
    /// itself.
    /// @param size             Panel extent in world units: x the arc length across the curve, y the straight height.
    /// @param curvatureRadius  Radius of the cylinder the panel is a section of, in world units.
    /// @param subdivisions     Grid cells per axis across the panel; clamped to at least 1 each.
    /// @param material         Material recorded on the produced submesh; empty leaves it unassigned.
    /// @pre `curvatureRadius > 0` and both components of `size` are > 0.
    /// @see CurvedPanelHit, CurvedPanelSizeForRect
    [[nodiscard]] MeshData CurvedPanel(vec2 size, f32 curvatureRadius, uvec2 subdivisions,
                                       AssetHandle<MaterialInstance> material = {});

    /// @brief Panel UV where a panel-space ray meets a curved panel's front face.
    ///
    /// The inverse of CurvedPanel's parameterization, in that panel's own local frame — origin at the
    /// surface centre, normal +Z, centre of curvature at `(0, 0, +curvatureRadius)`. It is what makes
    /// a curved display usable for anything world-anchored — a marker, a reticle, a label over a thing
    /// in the scene: transform the eye and the target into panel space and the returned UV is the
    /// document coordinate to draw at. Without it a consumer hand-rolls a ray-cylinder intersection,
    /// and the two disagree about the panel's parameterization at exactly the moment a marker drifts
    /// off its target.
    ///
    /// Three properties of the solve, each of which the obvious implementation gets wrong:
    ///
    /// - **The nearer of the two cylinder roots *that lies on the panel* wins, not simply the nearer
    ///   root.** A ray can enter the infinite cylinder outside the panel's arc or above its height
    ///   and still reach the panel on the far root — the ordinary case for an eye more than
    ///   `2·curvatureRadius` from the axis, where the whole panel sits behind the cylinder's near
    ///   flank. Solving only the nearer root produces a marker that vanishes near the panel's edges,
    ///   which is easy to mistake for correct culling.
    /// - **The front face only.** A panel is a display and its back is not a place to draw. The near
    ///   root of a ray entering the cylinder always presents the back, so rejecting it is also what
    ///   lets the far root be found.
    /// - **A UV, never a world point.** The consumer wants the document coordinate; the world point
    ///   is `origin + t·direction` and was already recoverable.
    /// @param size             Panel extent as passed to CurvedPanel: x the arc length, y the straight height.
    /// @param curvatureRadius  Cylinder radius as passed to CurvedPanel.
    /// @param origin           Ray origin, in the panel's local space.
    /// @param direction        Ray direction, in the panel's local space; need not be normalized.
    /// @return The panel UV — top-left origin, y-down, matching CurvedPanel — of the front-face hit
    ///         at the smallest `t > 0`, or `nullopt` when the ray reaches no part of the panel: it
    ///         misses the cylinder, its intersections fall outside the arc or the height, it runs
    ///         parallel to the axis, or it meets only the back.
    /// @pre `curvatureRadius > 0` and both components of `size` are > 0.
    /// @warning The mapping inverts the generator only while the panel does not wrap the cylinder
    ///          (`size.x <= 2·pi·curvatureRadius`). A wrapped panel's overlapping arcs share angles
    ///          about the axis, so the returned UV names the position within the first turn.
    /// @see CurvedPanel
    [[nodiscard]] optional<vec2> CurvedPanelHit(vec2 size, f32 curvatureRadius, vec3 origin,
                                                vec3 direction);

    /// @brief Curved-panel size whose edges land on the edge rays of a normalized screen rect.
    ///
    /// The bridge between "this display should cover 85 % of the view's width" and the physical
    /// metres CurvedPanel takes. A panel of `curvatureRadius`, centred `distance` ahead of an eye on
    /// its own normal (the eye on the panel's +Z, looking down the panel's −Z), has exactly the
    /// returned `size` when its edges sit on the edge rays of the boresight-centred screen rect
    /// `rectSize` — in `[0,1]` window fractions, the same fractions ProjectionShell takes. It lives
    /// here rather than in a consumer because it has a closed form that is not obvious and is easy to
    /// get wrong by iteration.
    ///
    /// The derivation, so the implementation is checkable rather than trusted. Put the eye at the
    /// origin looking down −Z, write `d` for `distance` and `R` for `curvatureRadius`, and place the
    /// panel's centre at `(0, 0, −d)` with its axis of curvature through `(0, y, −d + R)`. A surface
    /// point at arc angle `φ` from the centre is
    ///
    ///     P(φ) = ( R·sin φ,  y,  −d + R − R·cos φ )
    ///
    /// and it lies on the ray of screen tangent `t` when `P.x / (−P.z) = t`, i.e.
    /// `R·sin φ = t·(d − R) + t·R·cos φ`. Collecting the two sinusoids as a single phase —
    /// `R·sin φ − t·R·cos φ = R·√(1+t²)·sin(φ − α)` with `α = atan(t)` — and using
    /// `t/√(1+t²) = sin α` gives the half-arc directly:
    ///
    ///     φ_edge = α + asin( (d − R)·sin α / R ),      α = atan( rectSize.x·aspect·tan(fovY/2) )
    ///
    /// so `size.x = 2·R·φ_edge`. The height needs no solve, being the flat case: the panel is straight
    /// along +Y, so at its centre column `size.y = 2·d·tan(fovY/2)·rectSize.y`. Away from that column
    /// the surface bows toward the eye, so the top and bottom edges project *outside* the rect at the
    /// flanks — that bow is what a curved panel is, not an error in the solve.
    ///
    /// Three regimes, all supported: `R = d` is the eye-centred arc, where `φ_edge = α` and the width
    /// is `2·d·atan(t)` measured along the arc; `R ≫ d` tends to the flat `2·d·t`; and `R < d` — the
    /// centre of curvature in front of the eye, a panel wrapping more tightly than an eye-centred arc
    /// — merely takes the `asin` argument positive and is bounded there like the others.
    ///
    /// **An unsolvable combination clamps, and never returns a NaN.** The `asin` argument exceeds 1
    /// exactly when the eye lies outside the cylinder (`R < d/2`) and the rect's edge ray misses that
    /// cylinder altogether — the curvature closes the panel off before it reaches the requested rect.
    /// The returned width is then the widest the curvature admits: the arc out to the surface's
    /// silhouette as seen from the eye, `φ_edge = asin(R/(d − R)) + pi/2`, which agrees with the
    /// solved branch at the boundary. The height is unaffected, so a clamped result is a panel
    /// narrower than requested rather than a degenerate one.
    /// @param fovY             Vertical field of view in radians of the projection the rect is expressed against.
    /// @param aspect           Viewport width divided by height of that same projection.
    /// @param rectSize         Size of the covered screen rect, in [0,1] window fractions, centred on the boresight.
    /// @param distance         Distance from the eye to the panel's surface centre, in world units.
    /// @param curvatureRadius  Radius of the cylinder the panel is a section of, in world units.
    /// @return The `size` to hand CurvedPanel: x the arc length across the curve, y the straight height.
    /// @pre `aspect > 0`, `0 < fovY < pi`, `distance > 0`, `curvatureRadius > 0`, and both components
    ///      of `rectSize` are > 0.
    /// @see CurvedPanel
    [[nodiscard]] vec2 CurvedPanelSizeForRect(f32 fovY, f32 aspect, vec2 rectSize, f32 distance,
                                              f32 curvatureRadius);

    /// @brief UV sphere of `radius` with `rings` latitude bands and `segments` longitude bands (min 3 each).
    ///
    /// Smooth normals; UVs are (longitude, latitude). One submesh.
    [[nodiscard]] MeshData Sphere(f32 radius = 0.5f, u32 rings = 16, u32 segments = 32,
                                  AssetHandle<MaterialInstance> material = {});

    /// @brief Geodesic sphere of `radius` from a subdivided icosahedron.
    ///
    /// Each of the 20 base faces is split `subdivisions` times (4^subdivisions triangles each)
    /// and projected onto the sphere, so vertices are near-uniformly distributed with no pole
    /// clustering. Smooth normals; equirectangular UVs with the wrap seam split to avoid smearing.
    /// One submesh.
    [[nodiscard]] MeshData Icosphere(f32 radius = 0.5f, u32 subdivisions = 3,
                                     AssetHandle<MaterialInstance> material = {});

    /// @brief Capped cylinder about the Y axis, `radius` across and `height` tall, centered at the origin.
    ///
    /// A radial side band of `segments` longitude columns (min 3) plus a top and bottom cap fan,
    /// each with its own hard +Y / -Y normal. Side normals point radially outward; side UVs are
    /// (longitude, height). One submesh.
    [[nodiscard]] MeshData Cylinder(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 32,
                                    AssetHandle<MaterialInstance> material = {});

    /// @brief Cone about the Y axis: a circular base of `radius` and an apex `height` above it, centered at the origin.
    ///
    /// A side band of `segments` longitude columns (min 3) whose apex ring duplicates the apex
    /// per segment so each side face carries its own slanted normal, plus a bottom cap fan with a
    /// hard -Y normal. One submesh.
    [[nodiscard]] MeshData Cone(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 32,
                                AssetHandle<MaterialInstance> material = {});

    /// @brief Torus in the XZ plane: a tube of `minorRadius` swept around `majorRadius`, centered at the origin.
    ///
    /// `majorSegments` columns around the ring and `minorSegments` columns around the tube (min 3
    /// each); seam columns duplicate so UVs do not wrap. Smooth normals point away from the tube
    /// center circle; UVs are (major angle, minor angle). One submesh.
    [[nodiscard]] MeshData Torus(f32 majorRadius = 0.5f, f32 minorRadius = 0.2f,
                                 u32 majorSegments = 32, u32 minorSegments = 16,
                                 AssetHandle<MaterialInstance> material = {});

    /// @brief Flat annulus in the XZ plane (+Y normal): a ring of `innerRadius` to `outerRadius`, centered at the origin.
    ///
    /// The one flat closed shape with a hole — the companion of Plane's flat sheet and of the
    /// Torus that sweeps a tube around the same profile. Tessellated into `radialSegments` x
    /// `angularSegments` quads, one submesh by default.
    ///
    /// **UVs are radial**, which is the reason to generate the shape rather than mask a quad: `u`
    /// is the normalized radial fraction across the ring (0 at the inner edge, 1 at the outer), so
    /// a radial profile is a 1D texture lookup or a 1D function of `u`, and `v` is the angular
    /// fraction around the ring, so wrapping a texture around it is a `v` repeat. The angular seam
    /// column duplicates at 0 and a full turn, so `v` spans [0,1] rather than wrapping to 0 at the
    /// seam. Tangents follow +U, radially outward.
    ///
    /// The winding is counter-clockwise seen from +Y, matching Plane, so the back-face default
    /// culls both undersides identically. The annulus is single-sided in exactly the sense Plane
    /// is: a two-sided ring is the material's CullMode::None, not a second geometry variant.
    ///
    /// `angularSubmeshes` (min 1) splits the ring into that many equal angular sectors, each its
    /// own submesh sharing the one material. It partitions the **index buffer**, not the vertices
    /// — the sectors share one vertex grid — and `angularSegments` is raised to the next multiple
    /// of it so the sectors are equal and no quad straddles a boundary. One submesh, the default,
    /// is the ordinary case; a translucent annulus wants several, because a renderer that sorts
    /// translucent geometry per submesh cannot order a single-submesh ring against anything
    /// concentric with it — every part of the ring carries the same view depth, while its near and
    /// far arcs need opposite orders. Sectors give the sorter something to work with and cost one
    /// draw each.
    ///
    /// A zero `innerRadius` is legal and gives a filled disc; its innermost band is a centre fan,
    /// where one triangle per quad collapses to zero area at the origin. An `innerRadius` past
    /// `outerRadius` is swapped rather than producing inverted geometry.
    /// @param innerRadius       Radius of the hole, in units; 0 gives a filled disc.
    /// @param outerRadius       Radius of the outer edge, in units.
    /// @param radialSegments    Quad bands across the ring; clamped to at least 1.
    /// @param angularSegments   Quad columns around the ring; clamped to at least 3, then raised
    ///                          to the next multiple of `angularSubmeshes`.
    /// @param angularSubmeshes  Equal angular sectors, one submesh each; clamped to at least 1.
    /// @param material          Material recorded on the produced submeshes; empty leaves them unassigned.
    /// @return CPU-side MeshData in the canonical vertex layout.
    [[nodiscard]] MeshData Annulus(f32 innerRadius = 0.25f, f32 outerRadius = 0.5f,
                                   u32 radialSegments = 1, u32 angularSegments = 32,
                                   u32 angularSubmeshes = 1,
                                   AssetHandle<MaterialInstance> material = {});

    /// @brief Capsule about the Y axis: a cylinder of `height` capped by two hemispheres of `radius`, centered at the origin.
    ///
    /// `segments` longitude columns (min 3) shared by the band and both caps; each hemisphere has
    /// `rings` latitude bands (min 1). The cylinder spans the central `height`; the hemisphere
    /// centers sit at ±height/2, so the full extent is height + 2*radius. Smooth normals; one submesh.
    [[nodiscard]] MeshData Capsule(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 32,
                                   u32 rings = 8, AssetHandle<MaterialInstance> material = {});
}
