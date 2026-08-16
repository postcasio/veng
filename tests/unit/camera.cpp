// Camera math: the view/projection construction the scene renderer draws
// through. Pure CPU — no Context, no Vulkan symbol touched. Pins the engine's
// clip conventions (the Vulkan Y-flip), the near/far the camera carries
// alongside the projection, position recovery from the view's inverse, and the
// physical-lens arm — field-of-view derivation, the millimetre/metre
// normalization, and the circle-of-confusion curve its constants define.

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Camera.h>

using namespace Veng;

namespace
{
    bool VecApprox(const vec3& a, const vec3& b, f32 eps = 1e-4f)
    {
        return glm::all(glm::lessThan(glm::abs(a - b), vec3(eps)));
    }

    bool MatrixApprox(const mat4& a, const mat4& b, f32 eps = 1e-4f)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                if (std::abs(a[c][r] - b[c][r]) > eps)
                {
                    return false;
                }
            }
        }
        return true;
    }
}

TEST_CASE("SetPerspective flips Y for Vulkan clip space and carries near/far")
{
    CameraView camera;
    camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.25f, 250.0f);

    // glm::perspective yields a positive [1][1]; the engine negates it so clip
    // space has Y pointing down. The flip is the whole point — assert its sign.
    CHECK(camera.Projection()[1][1] < 0.0f);

    // The camera carries the range SetPerspective was given (reconstruction from
    // the matrix is fiddly under the flip, so it is stored, not derived).
    CHECK(camera.GetNear() == doctest::Approx(0.25f));
    CHECK(camera.GetFar() == doctest::Approx(250.0f));
}

TEST_CASE("SetOrthographic flips Y for Vulkan clip space and carries near/far")
{
    CameraView camera;
    camera.SetOrthographic(4.0f, 3.0f, 0.5f, 50.0f);

    // Same Y-flip as the perspective path: glm::ortho yields a positive [1][1],
    // the engine negates it so clip space has Y pointing down.
    CHECK(camera.Projection()[1][1] < 0.0f);

    CHECK(camera.GetNear() == doctest::Approx(0.5f));
    CHECK(camera.GetFar() == doctest::Approx(50.0f));
}

TEST_CASE("Orthographic projection has no perspective foreshortening")
{
    CameraView camera;
    camera.SetOrthographic(2.0f, 2.0f, 0.1f, 100.0f);
    camera.SetView(vec3{0.0f, 0.0f, 5.0f}, vec3{0.0f}, vec3{0.0f, 1.0f, 0.0f});

    // A unit-width span projects to the same clip-X extent whether it sits near
    // or far from the camera — the defining property of a parallel projection.
    const auto clipX = [&](vec3 world)
    {
        const vec4 clip = camera.ViewProjection() * vec4{world, 1.0f};
        return clip.x / clip.w;
    };

    const f32 nearSpan = clipX(vec3{1.0f, 0.0f, 2.0f}) - clipX(vec3{-1.0f, 0.0f, 2.0f});
    const f32 farSpan = clipX(vec3{1.0f, 0.0f, -3.0f}) - clipX(vec3{-1.0f, 0.0f, -3.0f});
    CHECK(nearSpan == doctest::Approx(farSpan));
}

TEST_CASE("Y-flip maps a world-up point to negative clip Y")
{
    CameraView camera;
    camera.SetPerspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3{0.0f}, vec3{0.0f, 0.0f, -1.0f}, vec3{0.0f, 1.0f, 0.0f});

    // A point above the view centre and in front of the camera. Under the
    // Vulkan Y-flip, up projects to negative clip Y (it would be positive
    // without the flip — this is what the flip exists to correct).
    const vec4 up = camera.ViewProjection() * vec4{0.0f, 1.0f, -2.0f, 1.0f};
    CHECK(up.w > 0.0f);
    CHECK(up.y / up.w < 0.0f);

    // A point on the view axis projects to clip-Y zero.
    const vec4 centre = camera.ViewProjection() * vec4{0.0f, 0.0f, -2.0f, 1.0f};
    CHECK(centre.y / centre.w == doctest::Approx(0.0f));
}

TEST_CASE("SetView places the camera; GetPosition recovers the eye")
{
    CameraView camera;
    const vec3 eye{3.0f, 4.0f, 5.0f};
    camera.SetView(eye, vec3{0.0f}, vec3{0.0f, 1.0f, 0.0f});

    CHECK(VecApprox(camera.GetPosition(), eye));
}

TEST_CASE("SetViewFromWorld is the world matrix's inverse and recovers position")
{
    const vec3 eye{-2.0f, 7.0f, 1.5f};
    const mat4 world = glm::translate(mat4{1.0f}, eye) *
                       glm::rotate(mat4{1.0f}, glm::radians(35.0f), vec3{0.0f, 1.0f, 0.0f});

    CameraView camera;
    camera.SetViewFromWorld(world);

    CHECK(MatrixApprox(camera.View(), glm::inverse(world)));
    // The position is the world's translation column regardless of rotation.
    CHECK(VecApprox(camera.GetPosition(), eye));
}

TEST_CASE("the camera's right and up axes are the view's rows, not its columns")
{
    // A rotation about all three axes, so the view's rows and columns differ in every component —
    // an axis-aligned pose cannot tell a transposed basis from a correct one.
    const mat4 world =
        glm::translate(mat4{1.0f}, vec3{2.0f, -1.0f, 4.0f}) *
        glm::eulerAngleYXZ(glm::radians(37.0f), glm::radians(-21.0f), glm::radians(14.0f));

    CameraView camera;
    camera.SetViewFromWorld(world);

    // The world matrix's basis columns are the camera's axes in world space, which is what the
    // view's rows recover.
    CHECK(VecApprox(camera.GetRight(), vec3(world[0])));
    CHECK(VecApprox(camera.GetUp(), vec3(world[1])));

    // The transposed read is orthonormal too, which is why it fails silently rather than loudly:
    // assert the basis is the one that actually tracks the pose.
    const mat4 view = camera.View();
    CHECK(glm::length(vec3(view[0][0], view[0][1], view[0][2]) - camera.GetRight()) > 0.1f);
}

TEST_CASE("ViewProjection composes Projection * View")
{
    CameraView camera;
    camera.SetPerspective(glm::radians(50.0f), 4.0f / 3.0f, 0.1f, 80.0f);
    camera.SetView(vec3{1.0f, 2.0f, 6.0f}, vec3{0.0f}, vec3{0.0f, 1.0f, 0.0f});

    CHECK(MatrixApprox(camera.ViewProjection(), camera.Projection() * camera.View()));
}

TEST_CASE("MakeCameraView composes a Camera component, aspect, and world matrix")
{
    Camera component;
    component.FovY = glm::radians(70.0f);
    component.Near = 0.2f;
    component.Far = 120.0f;

    const f32 aspect = 16.0f / 9.0f;
    const vec3 eye{0.0f, 3.0f, 9.0f};
    const mat4 world = glm::translate(mat4{1.0f}, eye);

    const CameraView made = MakeCameraView(component, aspect, world);

    CameraView expected;
    expected.SetPerspective(component.FovY, aspect, component.Near, component.Far);
    expected.SetViewFromWorld(world);

    CHECK(MatrixApprox(made.Projection(), expected.Projection()));
    CHECK(MatrixApprox(made.View(), expected.View()));
    CHECK(VecApprox(made.GetPosition(), eye));
    CHECK(made.GetNear() == doctest::Approx(component.Near));
    CHECK(made.GetFar() == doctest::Approx(component.Far));
}

TEST_CASE("MakeCameraView resolves an Orthographic component through SetOrthographic")
{
    // OrthoHeight is the full vertical extent; the half-width follows the aspect.
    Camera component;
    component.Projection = CameraProjection::Orthographic;
    component.OrthoHeight = 9.0f;
    component.Near = 0.5f;
    component.Far = 60.0f;

    const f32 aspect = 16.0f / 9.0f;
    const vec3 eye{2.0f, 1.0f, 12.0f};
    const mat4 world = glm::translate(mat4{1.0f}, eye);

    const CameraView made = MakeCameraView(component, aspect, world);

    CameraView expected;
    expected.SetOrthographic(4.5f * aspect, 4.5f, component.Near, component.Far);
    expected.SetViewFromWorld(world);

    CHECK(MatrixApprox(made.Projection(), expected.Projection()));
    CHECK(MatrixApprox(made.View(), expected.View()));
    CHECK(VecApprox(made.GetPosition(), eye));
    CHECK(made.GetNear() == doctest::Approx(component.Near));
    CHECK(made.GetFar() == doctest::Approx(component.Far));
}

TEST_CASE("ProjectToScreen maps world points to top-left-origin pixels and rejects behind-camera")
{
    // Looking down -Z from the origin: world center projects to the extent's center, a point
    // above center lands in the upper half (top-left origin), and a point behind returns nullopt.
    CameraView camera;
    camera.SetPerspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));

    const vec2 extent(200.0f, 200.0f);

    const optional<vec2> center = ProjectToScreen(camera, vec3(0.0f, 0.0f, -10.0f), extent);
    REQUIRE(center.has_value());
    CHECK(center->x == doctest::Approx(100.0f));
    CHECK(center->y == doctest::Approx(100.0f));

    // At fovY 90° and distance 10, a point 10 up sits exactly on the top edge (y = 0).
    const optional<vec2> above = ProjectToScreen(camera, vec3(0.0f, 10.0f, -10.0f), extent);
    REQUIRE(above.has_value());
    CHECK(above->x == doctest::Approx(100.0f));
    CHECK(above->y == doctest::Approx(0.0f));

    CHECK(!ProjectToScreen(camera, vec3(0.0f, 0.0f, 10.0f), extent).has_value());
}

TEST_CASE("background view-direction reconstruction survives extreme Far/Near ratios")
{
    // The sky, TAA, and volume-field shaders reconstruct a background pixel's view ray as
    // normalize(InvViewProj·(ndc, 1, 1).xyz − camera·w) — the homogeneous form, whose direction
    // is independent of the chosen NDC z. Under reverse-Z the NDC z = 1 the shaders pass is the
    // *near* plane, which reconstructs well-conditioned however extreme Far/Near grows — the
    // f32 w that collapsed to zero under forward-Z (at the far plane, once Far/Near passes ~2^24)
    // does not collapse here. Pins both: the reconstructed direction stays exact, and the w stays
    // bounded away from zero so even the divided form is usable.
    const vec3 eye{0.0f, 1.5f, 4.0f};
    const vec3 target{-1.0f, 1.5f, 4.0f};
    const vec2 ndc{0.30f, -0.20f};
    const f32 far = 20000.0f;

    // The f64 reference direction, from unrounded double-precision matrices end to end. The
    // direction through a pixel is a property of the pixel, not the depth convention, so a
    // plain forward-Z reference gives the same ray the reverse-Z pipeline reconstructs.
    const auto reference = [&](f64 near) -> glm::dvec3
    {
        glm::dmat4 proj = glm::perspective(glm::radians(60.0), 16.0 / 9.0, near, f64{far});
        proj[1][1] *= -1.0;
        const glm::dmat4 view =
            glm::lookAt(glm::dvec3(eye), glm::dvec3(target), glm::dvec3(0.0, 1.0, 0.0));
        const glm::dvec4 worldH = glm::inverse(proj * view) * glm::dvec4(ndc, 1.0, 1.0);
        return glm::normalize(glm::dvec3(worldH) - glm::dvec3(eye) * worldH.w);
    };

    // The f32 pipeline exactly as the renderer and shaders evaluate it: a CameraView
    // projection (reverse-Z), an f32 matrix inverse, and the shader's reconstruction expressions.
    const auto reconstruct = [&](f32 ratio)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, far / ratio, far);
        camera.SetView(eye, target, vec3(0.0f, 1.0f, 0.0f));
        const mat4 inv = glm::inverse(camera.ViewProjection());
        return vec4{inv * vec4(ndc, 1.0f, 1.0f)};
    };
    const auto degreesOff = [&](vec3 dir, glm::dvec3 truth)
    { return glm::degrees(std::acos(glm::clamp(glm::dot(glm::dvec3(dir), truth), -1.0, 1.0))); };

    // The direction is reconstructed from the reverse-Z near-plane point (NDC z = 1), whose
    // world position is a small difference near the eye — an f32 cancellation the far-plane form
    // avoided — so the bound is a fraction of a degree rather than the 0.01 the far form held.
    // Still far below any visible offset, and the property is robustness across the ratios.
    constexpr f64 MaxOffDegrees = 0.05;

    // Ordinary ratio: both forms agree with the reference.
    {
        const f32 ratio = 1.0e4f;
        const vec4 worldH = reconstruct(ratio);
        const glm::dvec3 truth = reference(f64{far} / f64{ratio});
        const vec3 divided = glm::normalize(vec3(worldH) / worldH.w - eye);
        const vec3 homogeneous = glm::normalize(vec3(worldH) - eye * worldH.w);
        CHECK(degreesOff(divided, truth) < MaxOffDegrees);
        CHECK(degreesOff(homogeneous, truth) < MaxOffDegrees);
    }

    // Extreme ratio: reverse-Z samples the near plane (NDC z = 1), which stays well-conditioned,
    // so w does not collapse and the homogeneous form reconstructs the direction exactly.
    {
        const f32 ratio = 6.7e7f;
        const vec4 worldH = reconstruct(ratio);
        const glm::dvec3 truth = reference(f64{far} / f64{ratio});
        CHECK(std::abs(worldH.w) > 1.0e-3f);
        const vec3 homogeneous = glm::normalize(vec3(worldH) - eye * worldH.w);
        CHECK(degreesOff(homogeneous, truth) < MaxOffDegrees);
    }
}

// ---- The physical lens arm ---------------------------------------------------

namespace
{
    // The vertical field of view MakeCameraView derives, in degrees, for a lens/sensor pair.
    f32 PhysicalFovYDegrees(f32 focalLengthMm, f32 sensorHeightMm)
    {
        Camera component;
        component.Projection = CameraProjection::Physical;
        component.FocalLength = focalLengthMm;
        component.SensorHeight = sensorHeightMm;

        // Recover fovY from the projection's [1][1]: it is -cot(fovY/2) / aspect under the
        // engine's Y-flip, so at aspect 1 the half-angle is atan(1 / |[1][1]|).
        const CameraView made = MakeCameraView(component, 1.0f, mat4{1.0f});
        return glm::degrees(2.0f * std::atan(1.0f / std::abs(made.Projection()[1][1])));
    }
}

TEST_CASE("A physical camera derives its field of view from focal length over sensor height")
{
    // The full-frame reference pairs: a 50mm "normal" lens on a 24mm sensor is ~27 degrees
    // vertically, and a 24mm wide-angle on the same sensor is ~53.1.
    CHECK(PhysicalFovYDegrees(50.0f, 24.0f) == doctest::Approx(26.991f).epsilon(0.001));
    CHECK(PhysicalFovYDegrees(24.0f, 24.0f) == doctest::Approx(53.130f).epsilon(0.001));

    // The derivation is a ratio, so the same lens on a larger sensor sees wider.
    CHECK(PhysicalFovYDegrees(50.0f, 36.0f) > PhysicalFovYDegrees(50.0f, 24.0f));
}

TEST_CASE("A Physical camera matches a Perspective camera authored to the equivalent FovY")
{
    Camera physical;
    physical.Projection = CameraProjection::Physical;
    physical.FocalLength = 35.0f;
    physical.SensorHeight = 24.0f;
    physical.Near = 0.2f;
    physical.Far = 150.0f;

    Camera perspective = physical;
    perspective.Projection = CameraProjection::Perspective;
    perspective.FovY = 2.0f * std::atan(24.0f / (2.0f * 35.0f));

    const f32 aspect = 16.0f / 9.0f;
    const mat4 world = glm::translate(mat4{1.0f}, vec3{1.0f, 2.0f, 3.0f});

    const CameraView made = MakeCameraView(physical, aspect, world);
    const CameraView equivalent = MakeCameraView(perspective, aspect, world);

    CHECK(MatrixApprox(made.Projection(), equivalent.Projection()));
    CHECK(MatrixApprox(made.View(), equivalent.View()));
    CHECK(made.GetNear() == doctest::Approx(physical.Near));
    CHECK(made.GetFar() == doctest::Approx(physical.Far));

    // The lens block is the one thing that differs: it rides only the physical arm, so a
    // consumer reads its presence as "this view was authored in lens terms".
    CHECK(made.GetLens().has_value());
    CHECK_FALSE(equivalent.GetLens().has_value());
}

TEST_CASE("Only a Physical camera carries a lens, and it is normalized to metres")
{
    Camera component;
    component.Projection = CameraProjection::Physical;

    // The defaults: a 50mm f/2.8 lens on a 24mm sensor focused at 10 metres. Focal length and
    // sensor height are authored in millimetres, focus distance in metres — the mix the
    // normalization exists to resolve exactly once.
    const CameraLens lens = ComputeCameraLens(component);
    CHECK(lens.Aperture == doctest::Approx(0.0178571f));
    CHECK(lens.SensorHeight == doctest::Approx(0.024f));
    CHECK(lens.FocusDistance == doctest::Approx(10.0f));

    const CameraView made = MakeCameraView(component, 16.0f / 9.0f, mat4{1.0f});
    REQUIRE(made.GetLens().has_value());
    CHECK(made.GetLens()->Aperture == doctest::Approx(lens.Aperture));
    CHECK(made.GetLens()->SensorHeight == doctest::Approx(lens.SensorHeight));
    CHECK(made.GetLens()->FocusDistance == doctest::Approx(lens.FocusDistance));

    // A zeroed component still yields a finite lens rather than dividing by zero.
    Camera zeroed;
    zeroed.Projection = CameraProjection::Physical;
    zeroed.FocalLength = 0.0f;
    zeroed.SensorHeight = 0.0f;
    zeroed.FStop = 0.0f;
    zeroed.FocusDistance = 0.0f;
    const CameraLens floored = ComputeCameraLens(zeroed);
    CHECK(std::isfinite(floored.Aperture));
    CHECK(floored.SensorHeight > 0.0f);
    CHECK(floored.FocusDistance > 0.0f);
}

TEST_CASE("ComputeDofParams adds the sensor-to-pixel scale from the target's pixel height")
{
    Camera component;
    component.Projection = CameraProjection::Physical;

    const DofParams params = ComputeDofParams(component, 1080.0f);
    CHECK(params.Aperture == doctest::Approx(0.0178571f));
    CHECK(params.FocusDistance == doctest::Approx(10.0f));

    // A 24mm sensor mapped onto 1080 pixels: 1080 / 0.024 metres.
    CHECK(params.CocScale == doctest::Approx(45000.0f));

    // The scale — and only the scale — tracks the target's pixel height, which is why it is a
    // separate value: folding it into the aperture would make the aperture shift on a resize.
    const DofParams taller = ComputeDofParams(component, 2160.0f);
    CHECK(taller.CocScale == doctest::Approx(90000.0f));
    CHECK(taller.Aperture == doctest::Approx(params.Aperture));

    // The lens overload and the component overload agree.
    const DofParams viaLens = ComputeDofParams(ComputeCameraLens(component), 1080.0f);
    CHECK(viaLens.CocScale == doctest::Approx(params.CocScale));
    CHECK(viaLens.Aperture == doctest::Approx(params.Aperture));
}

TEST_CASE("The circle-of-confusion curve is zero in focus, signed, asymmetric, and bounded behind")
{
    Camera component;
    component.Projection = CameraProjection::Physical;
    const DofParams params = ComputeDofParams(component, 1080.0f);

    // In focus: no defocus at all.
    CHECK(ComputeCircleOfConfusion(params, 10.0f) == doctest::Approx(0.0f));

    // The sign carries the field: negative in front of the focus plane, positive behind it.
    CHECK(ComputeCircleOfConfusion(params, 5.0f) < 0.0f);
    CHECK(ComputeCircleOfConfusion(params, 20.0f) > 0.0f);

    // Asymmetric about the focus plane: halving the distance defocuses harder than doubling it,
    // because the near term grows without bound while the far term is bounded by one.
    const f32 near = std::abs(ComputeCircleOfConfusion(params, 5.0f));
    const f32 far = std::abs(ComputeCircleOfConfusion(params, 20.0f));
    CHECK(near > far);
    CHECK(near == doctest::Approx(2.0f * far));

    // Behind the focus plane the curve converges to the aperture-scaled constant.
    const f32 limit = params.CocScale * params.Aperture;
    CHECK(limit == doctest::Approx(803.571f).epsilon(0.001));
    CHECK(ComputeCircleOfConfusion(params, 1.0e6f) == doctest::Approx(limit).epsilon(0.001));
    CHECK(ComputeCircleOfConfusion(params, 1.0e4f) < limit);

    // A non-positive depth has no defocus defined.
    CHECK(ComputeCircleOfConfusion(params, 0.0f) == doctest::Approx(0.0f));
    CHECK(ComputeCircleOfConfusion(params, -3.0f) == doctest::Approx(0.0f));
}

TEST_CASE("A Physical camera round-trips through the reflection serializer")
{
    // The authoring round-trip cook, spawn, inspector edit, and save all share: the reflected
    // field set plus the enum's serialize-by-name convention. An authored Physical camera has
    // to survive it, and a record written before the mode existed has to still read.
    TypeRegistry registry;
    registry.Register<Camera>();
    const TypeInfo& info = registry.Info(registry.IdOf<Camera>());
    const JsonFieldHooks hooks;

    Camera src;
    src.Projection = CameraProjection::Physical;
    src.FocalLength = 85.0f;
    src.SensorHeight = 36.0f;
    src.FStop = 1.4f;
    src.FocusDistance = 2.5f;
    src.Near = 0.05f;
    src.Far = 500.0f;

    const nlohmann::json doc = JsonWriteFields(&src, info, registry, hooks);

    // The enum serializes by name, so the new enumerator needs no cooked-format bump.
    CHECK(doc.at("Projection") == "Physical");

    Camera dst;
    REQUIRE(JsonReadFields(&dst, info, doc, registry, hooks));
    CHECK(dst.Projection == CameraProjection::Physical);
    CHECK(dst.FocalLength == doctest::Approx(85.0f));
    CHECK(dst.SensorHeight == doctest::Approx(36.0f));
    CHECK(dst.FStop == doctest::Approx(1.4f));
    CHECK(dst.FocusDistance == doctest::Approx(2.5f));
    CHECK(dst.Near == doctest::Approx(0.05f));
    CHECK(dst.Far == doctest::Approx(500.0f));

    // An edit re-saves through the same path with no loss.
    dst.FocusDistance = 7.25f;
    const nlohmann::json edited = JsonWriteFields(&dst, info, registry, hooks);
    Camera reloaded;
    REQUIRE(JsonReadFields(&reloaded, info, edited, registry, hooks));
    CHECK(reloaded.FocusDistance == doctest::Approx(7.25f));
    CHECK(reloaded.Projection == CameraProjection::Physical);

    // A record predating the lens fields reads tolerantly, keeping the defaults.
    nlohmann::json legacy = nlohmann::json::object();
    legacy["Projection"] = "Perspective";
    legacy["FovY"] = 1.0f;
    Camera fromLegacy;
    REQUIRE(JsonReadFields(&fromLegacy, info, legacy, registry, hooks));
    CHECK(fromLegacy.Projection == CameraProjection::Perspective);
    CHECK(fromLegacy.FovY == doctest::Approx(1.0f));
    CHECK(fromLegacy.FocalLength == doctest::Approx(50.0f));
}
