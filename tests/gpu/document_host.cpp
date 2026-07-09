// DocumentHost (the pure document core) + DocumentLayer (the screen-space presenter). Cooks the UI
// HUD fixture pack, mounts it, and drives the split lifecycle:
//
//  DocumentHost:
//   - lazy instantiate: Get() is null until the first Drive, which loads + instantiates the tree;
//   - SetOnInstantiate fires exactly once on the lazy load and immediately on an already-live tree;
//   - Recreate re-instantiates a fresh tree and re-runs the on-instantiate hook;
//   - SetContext binds on instantiate — the bound {player.health} drives the rendered meter.
//
//  DocumentLayer:
//   - Present attaches the host's document to the viewport; dropping the layer detaches it without
//     destroying the tree (the host still owns it);
//   - a Recreate'd document re-attaches on the next Present;
//   - the interactivity flag reaches the document on attach.

#include <filesystem>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentHost.h>
#include <Veng/Gui/DocumentLayer.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>
#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The shared HUD fixture (tests/cooker/fixtures/ui_hud_pack.json): a full-region panel with one
    // bound, sized ProgressBar meter whose green fill tracks {player.health} — a binding->render
    // oracle and a real cooked UIDocument for the lifecycle cases.
    constexpr AssetId UIDocumentId{0xA09AA8B60AEAA8BEULL};
    constexpr uvec2 Extent{128, 96};

    // The HUD's view-model: the fixture binds {player.health}, so the bound object owns a `player`
    // struct with a `health` field. Distinct types/ids from ui_document_hud.cpp's HudModel.
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
        const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_document_host.vengpack";
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
            .Role = ViewportRole::Offscreen,
        });
    }

    // Downloads the RGBA16Sfloat viewport output and counts strongly-green pixels — the fixture
    // meter fill is saturated green, so its coverage tracks the bound {player.health}.
    usize GreenPixels(const Ref<ImageView>& output)
    {
        const vector<u8> raw = output->GetImage()->Download();
        const auto* halves = reinterpret_cast<const u16*>(raw.data());
        const usize pixels = static_cast<usize>(Extent.x) * Extent.y;
        usize count = 0;
        for (usize i = 0; i < pixels; ++i)
        {
            const f32 g = glm::unpackHalf1x16(halves[i * 4 + 1]);
            const f32 r = glm::unpackHalf1x16(halves[i * 4 + 0]);
            if (g > 0.5f && r < 0.3f)
            {
                ++count;
            }
        }
        return count;
    }
}

VE_REFLECT(::DocHostPlayer, 0x3A85022BEFA46393ULL)
VE_FIELD(health)
VE_REFLECT_END();

VE_REFLECT(::DocHostModel, 0xA22ED6670CBA50B9ULL)
VE_FIELD(player)
VE_REFLECT_END();

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "document host: lazy instantiate + SetOnInstantiate fires once and immediately")
{
    const path archive = CookUiPack();

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    Gui::DocumentHost host(assets, Types, UIDocumentId);

    int calls = 0;
    Gui::Document* seen = nullptr;
    host.SetOnInstantiate(
        [&](Gui::Document& document)
        {
            ++calls;
            seen = &document;
        });
    // Nothing is live yet, so neither the document nor the callback has run.
    CHECK(host.Get() == nullptr);
    CHECK(calls == 0);

    // The first Drive lazily loads, instantiates, binds, runs the callback once, and exposes the tree.
    const Gui::Document* const live = host.Drive();
    REQUIRE(live != nullptr);
    CHECK(host.Get() == live);
    CHECK(calls == 1);
    CHECK(seen == live);

    // A second Drive reuses the live tree — no re-instantiate, the callback does not fire again.
    CHECK(host.Drive() == live);
    CHECK(calls == 1);

    // Setting a callback on an already-live document runs it immediately, on that document.
    int second = 0;
    host.SetOnInstantiate([&](Gui::Document&) { ++second; });
    CHECK(second == 1);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "document host: Recreate re-instantiates a fresh tree and re-runs the hook")
{
    const path archive = CookUiPack();

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    Gui::DocumentHost host(assets, Types, UIDocumentId);
    int calls = 0;
    host.SetOnInstantiate([&](Gui::Document&) { ++calls; });

    REQUIRE(host.Drive() != nullptr);
    CHECK(calls == 1);

    // Recreate drops the live tree; the next Drive re-loads and re-instantiates a fresh one and
    // re-runs the hook, so a consumer's cached element pointers are refreshed. The re-run of the hook
    // (not pointer identity, which an allocator may reuse) is the re-instantiate oracle.
    host.Recreate();
    CHECK(host.Get() == nullptr);
    REQUIRE(host.Drive() != nullptr);
    CHECK(calls == 2);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "document layer: present attaches, drop detaches without destroying, interactive "
                  "flag reaches the document")
{
    RegisterBuiltinTypes(Types);

    const path archive = CookUiPack();

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    const Unique<Viewport> viewport = MakeViewport(Context, assets);
    Gui::DocumentHost host(assets, Types, UIDocumentId);

    const Gui::Document* live = nullptr;
    {
        Gui::DocumentLayer layer(host);
        layer.SetInteractive(true);

        live = layer.Present(*viewport);
        REQUIRE(live != nullptr);
        // Present attached the host's document to the viewport's layer stack, and the interactivity
        // flag reached the live document on that attach.
        CHECK(live->GetHostViewport() == viewport.get());
        CHECK(live->IsInteractive());

        // A second Present is a cheap no-op — already attached to the same viewport.
        CHECK(layer.Present(*viewport) == live);
        CHECK(live->GetHostViewport() == viewport.get());
    }

    // Dropping the layer detached the document from the viewport, but the host still owns the tree.
    CHECK(host.Get() == live);
    CHECK(live->GetHostViewport() == nullptr);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "document layer: a Recreate'd document re-attaches on the next present, display "
                  "only by default")
{
    RegisterBuiltinTypes(Types);

    const path archive = CookUiPack();

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    const Unique<Viewport> viewport = MakeViewport(Context, assets);
    Gui::DocumentHost host(assets, Types, UIDocumentId);
    int instantiations = 0;
    host.SetOnInstantiate([&](Gui::Document&) { ++instantiations; });
    Gui::DocumentLayer layer(host);

    const Gui::Document* const first = layer.Present(*viewport);
    REQUIRE(first != nullptr);
    CHECK(instantiations == 1);
    CHECK(first->GetHostViewport() == viewport.get());
    // No SetInteractive, so the document is display-only by default.
    CHECK_FALSE(first->IsInteractive());

    // The host recreates its tree; the next Present re-instantiates (the hook re-runs) and attaches
    // the fresh document to the viewport — the re-attach-on-recreation path.
    host.Recreate();
    const Gui::Document* const second = layer.Present(*viewport);
    REQUIRE(second != nullptr);
    CHECK(instantiations == 2);
    CHECK(second == host.Get());
    CHECK(second->GetHostViewport() == viewport.get());
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "document host: SetContext binds on instantiate and the bound value drives render")
{
    Types.Register<DocHostModel>();
    RegisterBuiltinTypes(Types);

    const path archive = CookUiPack();

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    DocHostModel model;
    model.player.health = 1.0f;
    Gui::BindingContext context;
    context.SetData(model);

    // The context is set before the document is live: the host binds it on instantiate (a deferred
    // bind, no lazy-attach ordering hole).
    Gui::DocumentHost host(assets, Types, UIDocumentId);
    host.SetContext(&context);

    const Unique<Scene> scene = Scene::Create(Types);
    const Unique<Viewport> viewport = MakeViewport(Context, assets);
    viewport->SetViewState({.World = scene.get(), .Delta = 0.016f});

    Gui::DocumentLayer layer(host);

    // Present drives the host (instantiate + bind + UpdateBindings) and attaches it; the engine then
    // composites the document. A full-health meter paints a wide green fill.
    REQUIRE(layer.Present(*viewport) != nullptr);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    const usize fullGreen = GreenPixels(viewport->GetOutput());
    CHECK(fullGreen > 0);

    // Move the bound value to half health; the next Present re-resolves the binding and the fill
    // shrinks — proving the SetContext bind took effect on the host-instantiated tree.
    model.player.health = 0.5f;
    context.Invalidate();
    REQUIRE(layer.Present(*viewport) != nullptr);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    const usize halfGreen = GreenPixels(viewport->GetOutput());
    CHECK(halfGreen < fullGreen);

    std::filesystem::remove(archive);
}
