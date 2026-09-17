// Input routing into a GuiOverlay's document, independent of where that document composites (GPU
// band): a viewport keeps an input attachment list separate from its layer stack, so a document
// blended into the scene HDR before bloom — which joins no layer stack — takes input on the same
// terms as one drawn after tonemap. The events travel the production path: InputRouter::Dispatch of
// a mouse event shaped as the window's GLFW callbacks shape it, offered to the router's consumer
// registry, where the real Gui::GuiConsumer maps the window point and drives the document.
//
// The band is GPU only because the routing goes through Renderer::Viewport and Viewport::Create
// needs a live Context (the gui_editing_keys reason); the behaviour under test is device-free.
//
// The cases pin:
//   (a) the window → document mapping at a non-zero region offset and a UI scale of 2 — a press
//       lands on the element the arithmetic says it should, and a press beside it lands on none;
//   (b) a display-only or hidden pre-bloom overlay is unreachable;
//   (c) precedence both ways: a post-tonemap document standing over a pre-bloom one is offered the
//       event first and absorbs it, and passes it down when it hits nothing;
//   (d) Interactive reaches a pre-bloom overlay's document, and flipping it off releases it.

#include <doctest/doctest.h>

#include <vector>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentHost.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/GuiConsumer.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Input.h>
#include <Veng/InputEvents.h>
#include <Veng/InputRouter.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // A region deliberately off the window origin and a UI scale of 2, so a mapping that drops
    // either term lands somewhere else: a window point maps to (point - Offset) / UiScale.
    constexpr ivec2 RegionOffset{64, 32};
    constexpr uvec2 RegionExtent{128, 128};
    constexpr f32 UiScale = 2.0f;

    // The hit target, in document logical points: the document is solved at 64 x 64 (the region
    // extent over the UI scale), and this rect covers x in [20, 50), y in [10, 30).
    constexpr vec2 TargetMin{20.0f, 10.0f};
    constexpr vec2 TargetSize{30.0f, 20.0f};

    // The window point that maps onto the middle of the target, and one that maps into the document
    // beside it — both inside the region, so the viewport hit-test itself is never what decides.
    constexpr ivec2 OnTarget{RegionOffset.x + 60, RegionOffset.y + 40};      // document (30, 20)
    constexpr ivec2 BesideTarget{RegionOffset.x + 110, RegionOffset.y + 40}; // document (55, 20)

    // The production routing stack for a mouse event: a headless snapshot, a router over the
    // context's viewport registry, one presented viewport, and the real GuiConsumer in the router's
    // consumer registry. With no window the content scale is 1, so a dispatched position is already
    // in window framebuffer pixels.
    struct PointerRoute
    {
        PointerRoute(Renderer::Context& context, AssetManager& assets)
            : Router(nullptr, Snapshot, context.GetViewportRegistry()),
              View(Viewport::Create({
                  .Context = context,
                  .Assets = assets,
                  .Region = {.Offset = RegionOffset, .Extent = RegionExtent},
                  .ColorFormat = Format::RGBA16Sfloat,
                  .Role = ViewportRole::Presented,
                  .UiScale = UiScale,
              })),
              Consumer(Router, Snapshot, nullptr, Viewports)
        {
            Viewports.push_back(View.get());
            Router.RegisterConsumer(Consumer);
        }

        // Presses the primary button at a window point, the way the window's GLFW callbacks shape
        // it: a move seeds the snapshot the consumer reads the position from, then the button press.
        void PressAt(ivec2 windowPoint)
        {
            MouseMovedEvent moved{vec2(windowPoint)};
            Router.Dispatch(moved);
            MouseButtonPressedEvent pressed{MouseButton::Left, 0};
            Router.Dispatch(pressed);
        }

        Input Snapshot{nullptr};
        InputRouter Router;
        std::vector<Viewport*> Viewports;
        Unique<Viewport> View;
        Gui::GuiConsumer Consumer;
    };

    // One clickable document: a single absolutely-placed button whose onPointer handler counts the
    // presses it consumes. The document is injected into the overlay's host rather than instantiated
    // from a recipe, so the hit geometry is stated here instead of read out of a cooked fixture.
    struct ClickTarget
    {
        Gui::BindingContext Context;
        Gui::Document* Doc = nullptr;
        Gui::Element* Button = nullptr;
        int Presses = 0;

        // Installs the document on the overlay's runtime host, which binds the context on adopt.
        void Install(GuiOverlay& overlay, vec2 min = TargetMin)
        {
            overlay.SetContext(&Context);
            Context.SetHandler("press", [this](Gui::Element&) { ++Presses; });

            Unique<Gui::Document> owned = CreateUnique<Gui::Document>();
            Doc = owned.get();

            // The full-bleed HUD posture: the root passes the pointer through, so the document
            // claims its widgets and nothing else — without it every press inside the document
            // extent is consumed by the root and no fall-through is observable.
            Gui::Style rootStyle = Doc->Root().BaseStyle;
            rootStyle.Pointer = Gui::PointerEvents::Children;
            Doc->SetStyle(Doc->Root(), rootStyle);

            Button = &Doc->Add(Doc->Root(), Gui::ElementKind::Button);
            Doc->SetPlacement(*Button, min, TargetSize);
            Button->Bindings["onPointer"] = "press";

            Gui::DocumentHost* const host = overlay.GetHost();
            REQUIRE(host != nullptr);
            host->SetDocument(std::move(owned));
        }
    };

    // Adds an overlay with no document recipe: the first drive materializes an injection-only host,
    // which ClickTarget::Install then feeds.
    GuiOverlay& AddOverlay(Scene& scene, Entity entity, GuiOverlayPlacement placement,
                           const bool interactive)
    {
        auto& overlay = scene.Add<GuiOverlay>(entity);
        overlay.Placement = placement;
        overlay.Interactive = interactive;
        return overlay;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui overlay input: a pre-bloom overlay's document takes the pointer")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);

    PointerRoute route(Context, assets);
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    GuiOverlay& overlay = AddOverlay(*scene, entity, GuiOverlayPlacement::SceneHdrPreBloom, true);

    // The first render materializes the host; the second drives the injected document.
    route.View->SetViewState({.World = scene.get(), .Delta = 0.016f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { route.View->Render(cmd); });
    ClickTarget target;
    target.Install(overlay);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { route.View->Render(cmd); });

    // The drive applied Interactive to the document and offered it to input, without attaching it
    // to the layer stack the compositor draws after tonemap.
    CHECK(target.Doc->IsInteractive());
    CHECK(route.View->GetAttachedDocuments().empty());
    REQUIRE(route.View->GetInputDocuments().size() == 1);
    CHECK(route.View->GetInputDocuments()[0] == target.Doc);

    // The mapping property: the window point the region offset and the UI scale place on the button
    // reaches it, and the one they place beside it reaches nothing.
    route.PressAt(OnTarget);
    CHECK(target.Presses == 1);

    route.PressAt(BesideTarget);
    CHECK(target.Presses == 1);

    // Gameplay's "is the UI using this pointer?" answers off the same list, so a pre-bloom
    // document's element owns the pointer exactly where it is drawn.
    CHECK(route.View->IsPointerOverDocument(OnTarget));
    CHECK_FALSE(route.View->IsPointerOverDocument(BesideTarget));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui overlay input: a display-only or hidden pre-bloom overlay is unreachable")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);

    PointerRoute route(Context, assets);
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    GuiOverlay& overlay = AddOverlay(*scene, entity, GuiOverlayPlacement::SceneHdrPreBloom, false);

    route.View->SetViewState({.World = scene.get(), .Delta = 0.016f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { route.View->Render(cmd); });
    ClickTarget target;
    target.Install(overlay);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { route.View->Render(cmd); });

    // Display-only: it draws and binds, and no pointer reaches it.
    CHECK_FALSE(target.Doc->IsInteractive());
    CHECK(route.View->GetInputDocuments().empty());
    route.PressAt(OnTarget);
    CHECK(target.Presses == 0);

    // Flipping the reflected field at runtime opens it on the next drive.
    overlay.Interactive = true;
    Context.ImmediateCommands([&](CommandBuffer& cmd) { route.View->Render(cmd); });
    CHECK(target.Doc->IsInteractive());
    CHECK(route.View->GetInputDocuments().size() == 1);
    route.PressAt(OnTarget);
    CHECK(target.Presses == 1);

    // A hidden overlay is not driven, so it leaves the list even though its document is still
    // flagged interactive: what is not drawn cannot be clicked.
    overlay.Visible = false;
    Context.ImmediateCommands([&](CommandBuffer& cmd) { route.View->Render(cmd); });
    CHECK(route.View->GetInputDocuments().empty());
    route.PressAt(OnTarget);
    CHECK(target.Presses == 1);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui overlay input: a post-tonemap document is offered the event above a "
                  "pre-bloom one")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);

    PointerRoute route(Context, assets);
    const Unique<Scene> scene = Scene::Create(Types);

    const Entity hdrEntity = scene->CreateEntity();
    AddOverlay(*scene, hdrEntity, GuiOverlayPlacement::SceneHdrPreBloom, true);
    const Entity ldrEntity = scene->CreateEntity();
    AddOverlay(*scene, ldrEntity, GuiOverlayPlacement::PostTonemap, true);

    route.View->SetViewState({.World = scene.get(), .Delta = 0.016f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { route.View->Render(cmd); });
    ClickTarget hdr;
    ClickTarget ldr;
    // Resolved after both components exist: adding the second may move the first in its pool.
    hdr.Install(scene->Get<GuiOverlay>(hdrEntity));
    ldr.Install(scene->Get<GuiOverlay>(ldrEntity));
    Context.ImmediateCommands([&](CommandBuffer& cmd) { route.View->Render(cmd); });

    // Composite order, bottom → top: the pre-bloom document is blended into the scene, the
    // post-tonemap one is drawn over the result.
    REQUIRE(route.View->GetInputDocuments().size() == 2);
    CHECK(route.View->GetInputDocuments()[0] == hdr.Doc);
    CHECK(route.View->GetInputDocuments()[1] == ldr.Doc);

    // Both documents carry a target at the same document point; the upper one absorbs the press.
    route.PressAt(OnTarget);
    CHECK(ldr.Presses == 1);
    CHECK(hdr.Presses == 0);

    // Move the upper document's target away: the press now passes through its root, and the lower
    // document takes it.
    ldr.Doc->SetPlacement(*ldr.Button, vec2(0.0f, 40.0f), TargetSize);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { route.View->Render(cmd); });
    route.PressAt(OnTarget);
    CHECK(ldr.Presses == 1);
    CHECK(hdr.Presses == 1);
}
