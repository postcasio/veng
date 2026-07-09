// GuiOverlay — the screen-space document component the Viewport discovers and drives. Cooks the UI
// HUD fixture pack, mounts it, and pins the engine-drive contract:
//
//   - component == hand-wired: an entity's GuiOverlay driven by the Viewport produces the
//     byte-identical composite the hand-wired DocumentHost + DocumentLayer path does;
//   - a runtime-removed component detaches its document (the drive-list reconciled);
//   - multi-viewport claim by seat: two Presented viewports on two seats each attach only their
//     seat's overlay, and an unbound overlay attaches to the sole/primary presenter;
//   - a failed document load surfaces as a null document, not an abort — the viewport still renders.

#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentHost.h>
#include <Veng/Gui/DocumentLayer.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>
#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The shared HUD fixture (tests/cooker/fixtures/ui_hud_pack.json): a full-region panel with one
    // bound ProgressBar meter whose green fill tracks {player.health}.
    constexpr AssetId UIDocumentId{0xA09AA8B60AEAA8BEULL};
    constexpr uvec2 Extent{128, 96};

    struct DocHostPlayer
    {
        f32 health = 0.0f;
    };

    struct DocHostModel
    {
        DocHostPlayer player;
    };

    path CookUiPack()
    {
        const path packJson = path(GPU_COOKER_FIXTURE_DIR) / "ui_hud_pack.json";
        const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_gui_overlay.vengpack";
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(cooker.CookPack(packJson, outArchive).has_value());
        return outArchive;
    }

    Unique<Viewport> MakeViewport(Context& context, AssetManager& assets)
    {
        return Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = Extent},
            .ColorFormat = Format::RGBA16Sfloat,
            .Role = ViewportRole::Presented,
        });
    }

    void RenderOnce(Context& context, Viewport& viewport)
    {
        context.ImmediateCommands([&](CommandBuffer& cmd) { viewport.Render(cmd); });
    }

    vector<u8> RenderOutput(Context& context, Viewport& viewport)
    {
        RenderOnce(context, viewport);
        return viewport.GetOutput()->GetImage()->Download();
    }

    // Reads a GuiOverlay's authored fields from JSON so a Document id can be set without an
    // AssetManager (the id lands in the handle's leading AssetId), for the failed-load case.
    JsonFieldHooks OverlayHooks()
    {
        JsonFieldHooks hooks;
        hooks.ReadReference = [](const nlohmann::json&) -> Result<Entity> { return Entity::Null; };
        hooks.WriteReference = [](Entity) -> nlohmann::json { return nlohmann::json(nullptr); };
        return hooks;
    }
}

VE_REFLECT(::DocHostPlayer, 0x1D6E9E2B0A4C7711ULL)
VE_FIELD(health)
VE_REFLECT_END();

VE_REFLECT(::DocHostModel, 0x2B7F0C4D1E5A8822ULL)
VE_FIELD(player)
VE_REFLECT_END();

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui overlay: the component-driven and hand-wired composites are byte-identical")
{
    Types.Register<DocHostModel>();
    RegisterBuiltinTypes(Types);

    const path archive = CookUiPack();
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    DocHostModel model;
    model.player.health = 1.0f;

    // The hand-wired path: a DocumentHost + DocumentLayer presented on a viewport, the pre-migration
    // wiring the component replaces.
    Gui::BindingContext handContext;
    handContext.SetData(model);
    const Unique<Scene> handScene = Scene::Create(Types);
    const Unique<Viewport> handViewport = MakeViewport(Context, assets);
    handViewport->SetViewState({.World = handScene.get(), .Delta = 0.016f});
    Gui::DocumentHost host(assets, Types, UIDocumentId);
    host.SetContext(&handContext);
    Gui::DocumentLayer layer(host);
    REQUIRE(layer.Present(*handViewport) != nullptr);
    const vector<u8> handOutput = RenderOutput(Context, *handViewport);

    // The component path: an entity's GuiOverlay, discovered and driven by the Viewport.
    Gui::BindingContext componentContext;
    componentContext.SetData(model);
    const Unique<Scene> componentScene = Scene::Create(Types);
    const Entity entity = componentScene->CreateEntity();
    auto& overlay = componentScene->Add<GuiOverlay>(entity);
    overlay.Document = *assets.LoadSync<Gui::UIDocument>(UIDocumentId);
    overlay.SetContext(&componentContext);
    const Unique<Viewport> componentViewport = MakeViewport(Context, assets);
    componentViewport->SetViewState({.World = componentScene.get(), .Delta = 0.016f});
    const vector<u8> componentOutput = RenderOutput(Context, *componentViewport);

    // The engine drive attached, bound, and composited the same document as the hand path — so the
    // two composites match byte for byte, the proof the component path removes only boilerplate.
    CHECK(componentOutput == handOutput);

    // And the driven overlay attached its live document to the component viewport's layer stack.
    REQUIRE(overlay.GetDocument() != nullptr);
    CHECK(overlay.GetDocument()->GetHostViewport() == componentViewport.get());
    CHECK(componentViewport->GetAttachedDocuments().size() == 1);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui overlay: a runtime-removed component detaches its document")
{
    RegisterBuiltinTypes(Types);

    const path archive = CookUiPack();
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    auto& overlay = scene->Add<GuiOverlay>(entity);
    overlay.Document = *assets.LoadSync<Gui::UIDocument>(UIDocumentId);

    const Unique<Viewport> viewport = MakeViewport(Context, assets);
    viewport->SetViewState({.World = scene.get(), .Delta = 0.016f});

    // The first render drives the overlay onto the layer stack.
    RenderOnce(Context, *viewport);
    CHECK(viewport->GetAttachedDocuments().size() == 1);

    // Removing the component destroys its runtime; the DocumentLayer detaches the document, so the
    // viewport's layer stack reconciles to empty with no dangling pointer.
    scene->Remove<GuiOverlay>(entity);
    CHECK(viewport->GetAttachedDocuments().empty());

    // The viewport still renders cleanly after the detach.
    RenderOnce(Context, *viewport);
    CHECK(viewport->GetAttachedDocuments().empty());

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui overlay: two seated viewports each claim only their seat's overlay, unbound "
                  "goes to the primary")
{
    RegisterBuiltinTypes(Types);

    const path archive = CookUiPack();
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    const Unique<Scene> scene = Scene::Create(Types);
    // Two seat entities and three overlays: one bound to each seat plus one unbound.
    const Entity seatA = scene->CreateEntity();
    const Entity seatB = scene->CreateEntity();

    // Configure each overlay right after its Add — a later Add may reallocate the component pool, so
    // the assertions below re-fetch through TryGet rather than holding these references.
    const Entity overlayAEntity = scene->CreateEntity();
    {
        auto& overlayA = scene->Add<GuiOverlay>(overlayAEntity);
        overlayA.Document = *assets.LoadSync<Gui::UIDocument>(UIDocumentId);
        overlayA.TargetSeat = seatA;
    }
    const Entity overlayBEntity = scene->CreateEntity();
    {
        auto& overlayB = scene->Add<GuiOverlay>(overlayBEntity);
        overlayB.Document = *assets.LoadSync<Gui::UIDocument>(UIDocumentId);
        overlayB.TargetSeat = seatB;
    }
    const Entity overlayUEntity = scene->CreateEntity();
    {
        auto& overlayU = scene->Add<GuiOverlay>(overlayUEntity);
        overlayU.Document = *assets.LoadSync<Gui::UIDocument>(UIDocumentId);
    }

    // A manual drive-list registering viewport A before B, so A is the primary presenter. The list
    // outlives the viewports, which self-erase from it on destruction (declared first).
    vector<Viewport*> driveList;
    const Unique<Viewport> viewportA = MakeViewport(Context, assets);
    const Unique<Viewport> viewportB = MakeViewport(Context, assets);

    driveList.push_back(viewportA.get());
    viewportA->AttachToDriveList(driveList);
    driveList.push_back(viewportB.get());
    viewportB->AttachToDriveList(driveList);

    viewportA->SetSeat(seatA);
    viewportB->SetSeat(seatB);
    viewportA->SetViewState({.World = scene.get(), .Delta = 0.016f});
    viewportB->SetViewState({.World = scene.get(), .Delta = 0.016f});

    RenderOnce(Context, *viewportA);
    RenderOnce(Context, *viewportB);

    const GuiOverlay* const overlayA = scene->TryGet<GuiOverlay>(overlayAEntity);
    const GuiOverlay* const overlayB = scene->TryGet<GuiOverlay>(overlayBEntity);
    const GuiOverlay* const overlayU = scene->TryGet<GuiOverlay>(overlayUEntity);
    REQUIRE(overlayA->GetDocument() != nullptr);
    REQUIRE(overlayB->GetDocument() != nullptr);
    REQUIRE(overlayU->GetDocument() != nullptr);

    // Each seated overlay attached only to its seat's viewport; nothing thrashes between the two.
    CHECK(overlayA->GetDocument()->GetHostViewport() == viewportA.get());
    CHECK(overlayB->GetDocument()->GetHostViewport() == viewportB.get());
    // The unbound overlay attached to the primary (first-registered) presenter, viewport A.
    CHECK(overlayU->GetDocument()->GetHostViewport() == viewportA.get());

    // Viewport A hosts its own seat's overlay plus the unbound one; B hosts only its seat's.
    CHECK(viewportA->GetAttachedDocuments().size() == 2);
    CHECK(viewportB->GetAttachedDocuments().size() == 1);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui overlay: a document that fails to load surfaces null, not an abort")
{
    RegisterBuiltinTypes(Types);

    const path archive = CookUiPack();
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    // Author a GuiOverlay referencing a valid-looking but unmounted document id (set through the
    // reflection read so the id lands in the handle without an AssetManager resolve).
    const nlohmann::json authored = {
        {"Document", "0xDEADBEEF0BADF00D"},
        {"Layer", 0},
        {"Interactive", false},
        {"TargetSeat", nullptr},
    };
    const TypeInfo& info = Types.Info(Types.IdOf<GuiOverlay>());
    GuiOverlay authoredOverlay;
    REQUIRE(JsonReadFields(&authoredOverlay, info, authored, Types, OverlayHooks()));

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity entity = scene->CreateEntity();
    scene->Add<GuiOverlay>(entity, std::move(authoredOverlay));

    const Unique<Viewport> viewport = MakeViewport(Context, assets);
    viewport->SetViewState({.World = scene.get(), .Delta = 0.016f});

    // Driving the overlay attempts the load, which fails and is logged once — the document stays
    // null and the viewport renders normally rather than aborting.
    RenderOnce(Context, *viewport);

    const GuiOverlay* const overlay = scene->TryGet<GuiOverlay>(entity);
    REQUIRE(overlay != nullptr);
    CHECK(overlay->GetDocument() == nullptr);
    CHECK(viewport->GetAttachedDocuments().empty());

    std::filesystem::remove(archive);
}
