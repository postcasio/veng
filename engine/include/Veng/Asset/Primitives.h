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

    /// @brief Capsule about the Y axis: a cylinder of `height` capped by two hemispheres of `radius`, centered at the origin.
    ///
    /// `segments` longitude columns (min 3) shared by the band and both caps; each hemisphere has
    /// `rings` latitude bands (min 1). The cylinder spans the central `height`; the hemisphere
    /// centers sit at ±height/2, so the full extent is height + 2*radius. Smooth normals; one submesh.
    [[nodiscard]] MeshData Capsule(f32 radius = 0.5f, f32 height = 1.0f, u32 segments = 32,
                                   u32 rings = 8, AssetHandle<MaterialInstance> material = {});
}
