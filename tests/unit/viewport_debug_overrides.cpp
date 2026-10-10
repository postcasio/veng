// FieldOverrides — a field-masked override set over a reflected struct — and the viewport debug
// override layer built from two of them. Pure struct copies, no device: an empty set changes
// nothing, a set field wins and nothing else moves, clearing returns the target's own value, the
// index a reflected-field UI drives agrees with the member-pointer form, and the view layer clamps
// the GPU loop bounds it can reach. Runs ICD-free.

#include <doctest/doctest.h>

#include <Veng/Reflection/FieldOverrides.h>
#include <Veng/Renderer/DofTile.h>
#include <Veng/Renderer/SsrLimits.h>
#include <Veng/Renderer/Viewport.h>

using namespace Veng;
using Renderer::SceneRendererSettings;
using Renderer::ViewState;

namespace
{
    // A view whose knobs are all off their defaults, standing in for a resolved look.
    ViewState ResolvedView()
    {
        ViewState view;
        view.Exposure = 2.0f;
        view.SsrMaxDistance = 9000.0f;
        view.SpecularAntiAliasingVariance = 0.3f;
        view.SpecularAntiAliasingThreshold = 0.25f;
        view.BloomIntensity = 1.5f;
        return view;
    }
}

TEST_CASE("An empty override set leaves its target exactly as it was")
{
    const FieldOverrides<SceneRendererSettings> overrides;
    CHECK(overrides.IsEmpty());
    CHECK(overrides.GetCount() == 0);

    SceneRendererSettings settings;
    settings.SSR = true;
    settings.ShadowResolution = 2048;
    const SceneRendererSettings before = settings;
    overrides.ApplyTo(settings);
    CHECK(settings == before);
}

TEST_CASE("A set field wins, every other field keeps the target's value, and clearing returns it")
{
    FieldOverrides<SceneRendererSettings> overrides;
    overrides.Set(&SceneRendererSettings::SSR, false);
    overrides.Set(&SceneRendererSettings::Mode, Renderer::DebugView::Normal);
    CHECK(overrides.GetCount() == 2);
    CHECK(overrides.IsSet(&SceneRendererSettings::SSR));
    CHECK_FALSE(overrides.IsSet(&SceneRendererSettings::Bloom));

    SceneRendererSettings resolved;
    resolved.SSR = true;
    resolved.Bloom = false;
    resolved.ShadowResolution = 512;

    SceneRendererSettings effective = resolved;
    overrides.ApplyTo(effective);
    CHECK_FALSE(effective.SSR);
    CHECK(effective.Mode == Renderer::DebugView::Normal);
    CHECK_FALSE(effective.Bloom);
    CHECK(effective.ShadowResolution == 512u);

    overrides.Clear(&SceneRendererSettings::SSR);
    effective = resolved;
    overrides.ApplyTo(effective);
    CHECK(effective.SSR);
    CHECK(effective.Mode == Renderer::DebugView::Normal);

    overrides.ClearAll();
    CHECK(overrides.IsEmpty());
    effective = resolved;
    overrides.ApplyTo(effective);
    CHECK(effective == resolved);
}

TEST_CASE("A field is addressed by its reflected index exactly as by its member")
{
    // The index is the descriptor's position in the type's reflected field list, which is what a
    // reflected-field UI hands back; setting by it copies only that field out of the edited copy.
    const vector<FieldDescriptor> fields = VengReflect<ViewState>::Fields();
    REQUIRE(fields.size() == FieldOverrides<ViewState>::GetFieldCount());

    FieldOverrides<ViewState> byMember;
    const usize index = byMember.IndexOf(&ViewState::SpecularAntiAliasingVariance);
    REQUIRE(index < fields.size());
    CHECK(fields[index].Name == "SpecularAntiAliasingVariance");

    ViewState edited = ResolvedView();
    edited.SpecularAntiAliasingVariance = 0.9f;
    edited.Exposure = 7.0f;

    FieldOverrides<ViewState> byIndex;
    byIndex.Set(index, edited);
    byMember.Set(&ViewState::SpecularAntiAliasingVariance, 0.9f);

    ViewState fromIndex = ResolvedView();
    byIndex.ApplyTo(fromIndex);
    ViewState fromMember = ResolvedView();
    byMember.ApplyTo(fromMember);

    CHECK(fromIndex.SpecularAntiAliasingVariance == 0.9f);
    CHECK(fromIndex.Exposure == ResolvedView().Exposure);
    CHECK(fromMember.SpecularAntiAliasingVariance == fromIndex.SpecularAntiAliasingVariance);
    CHECK(fromMember.Exposure == fromIndex.Exposure);
}

TEST_CASE("Every reflected settings field and view knob can be overridden")
{
    // Each field set on its own changes the target in that field: the set's table covers the
    // whole describe block, whatever the field's type.
    CHECK(FieldOverrides<SceneRendererSettings>::GetFieldCount() ==
          VengReflect<SceneRendererSettings>::Fields().size());
    CHECK(FieldOverrides<ViewState>::GetFieldCount() == VengReflect<ViewState>::Fields().size());

    SceneRendererSettings flipped;
    flipped.Bloom = !flipped.Bloom;
    flipped.Shadows = !flipped.Shadows;
    flipped.ShadowResolution += 1;
    flipped.CascadeSplitLambda += 0.1f;
    flipped.AntiAliasing = Renderer::AntiAliasingMode::TAA;
    flipped.SsrResolutionScale = SceneRendererSettings::SsrResolution::Full;
    flipped.Cull = SceneRendererSettings::CullMode::GPU;

    usize moved = 0;
    for (usize field = 0; field < FieldOverrides<SceneRendererSettings>::GetFieldCount(); ++field)
    {
        FieldOverrides<SceneRendererSettings> overrides;
        overrides.Set(field, flipped);
        SceneRendererSettings target;
        overrides.ApplyTo(target);
        moved += target == SceneRendererSettings{} ? 0 : 1;
    }
    CHECK(moved == 7);
}

TEST_CASE("The view override layer wins over a resolved view and clamps its GPU loop bounds")
{
    Renderer::ViewportDebugOverrides overrides;
    CHECK(overrides.IsEmpty());

    overrides.View.Set(&ViewState::SsrMaxDistance, 40.0f);
    overrides.View.Set(&ViewState::SsrMaxSteps, Renderer::SsrMaxStepsCeiling * 4u);
    overrides.View.Set(&ViewState::DofRingCount, Renderer::MaxDofRings * 4u);
    overrides.View.Set(&ViewState::DofMaxCoc, Renderer::DofCocCeiling * 4.0f);
    overrides.RenderScale = 0.5f;
    CHECK(overrides.GetCount() == 5);

    ViewState view = ResolvedView();
    overrides.ApplyTo(view);
    CHECK(view.SsrMaxDistance == 40.0f);
    CHECK(view.SsrMaxSteps == Renderer::SsrMaxStepsCeiling);
    CHECK(view.DofRingCount == Renderer::MaxDofRings);
    CHECK(view.DofMaxCoc == Renderer::DofCocCeiling);
    CHECK(view.Exposure == ResolvedView().Exposure);
    CHECK(view.SpecularAntiAliasingVariance == ResolvedView().SpecularAntiAliasingVariance);

    overrides.Clear();
    CHECK(overrides.IsEmpty());
    ViewState cleared = ResolvedView();
    overrides.ApplyTo(cleared);
    CHECK(cleared.SsrMaxDistance == ResolvedView().SsrMaxDistance);
}
