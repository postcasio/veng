// UI-document HUD end-to-end + editor cook-on-demand round-trip.
//
// (a) End-to-end HUD render: cooks the UIDocument + StyleSheet fixture pack, LoadSyncs the document,
//     Instantiates a live tree, binds a reflected view-model, attaches it to an Offscreen viewport,
//     and renders. Asserts the composited output carries the HUD's bound content — the ProgressBar
//     meter fill tracks the bound {player.health}, so a changed model value moves the visible fill.
//     This exercises the full cook -> load -> instantiate -> bind -> attach -> engine-drive ->
//     composite path a game HUD rides.
//
// (b) Cook-on-demand round-trip: cooks the same source into two independent in-memory archives at one
//     target id, mounts the first, loads + instantiates, then shadow-mounts the second over it and
//     re-fetches behind the stable id — the hot-swap-behind-a-stable-handle mechanism the editor
//     panel's live recook rides (RequestCook -> MountMemory -> reload behind the AssetHandle).

#include <cstring>
#include <filesystem>
#include <system_error>
#include <fstream>
#include <iterator>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/BindingContext.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/UIDocument.h>
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
    // The dedicated end-to-end fixture (tests/cooker/fixtures/ui_hud_pack.json): a full-region
    // panel with one bound, sized, saturated-green ProgressBar meter — a measurable
    // binding->render oracle whose green coverage tracks the bound {player.health}.
    constexpr AssetId UIDocumentId{0xA09AA8B60AEAA8BEULL};
    constexpr uvec2 Extent{128, 96};

    // The HUD's view-model: the fixture binds {player.health}, so the bound object owns a `player`
    // struct with a `health` field.
    struct HudPlayer
    {
        f32 health = 0.0f;
    };

    struct HudModel
    {
        HudPlayer player;
    };

    // Cooks the dedicated HUD fixture pack into a fresh archive on disk and returns its path.
    path CookUiPack()
    {
        const path packJson = path(GPU_COOKER_FIXTURE_DIR) / "ui_hud_pack.json";
        const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_ui_hud.vengpack";
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(cooker.CookPack(packJson, outArchive).has_value());
        return outArchive;
    }

    // Downloads the RGBA16Sfloat viewport output and counts the strongly-green pixels — the meter
    // fill is saturated green, so its coverage over the frame tracks the bound value.
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

VE_REFLECT(::HudPlayer, 0x7D1A9C4E6B02F358ULL)
VE_FIELD(health)
VE_REFLECT_END();

VE_REFLECT(::HudModel, 0x4C8F2A17E9B3065DULL)
VE_FIELD(player)
VE_REFLECT_END();

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ui document hud: cook -> load -> instantiate -> bind -> attach -> render")
{
    Types.Register<HudModel>();

    const path archive = CookUiPack();

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    const AssetResult<AssetHandle<Gui::UIDocument>> recipe =
        assets.LoadSync<Gui::UIDocument>(UIDocumentId);
    REQUIRE(recipe.has_value());
    REQUIRE(recipe->IsLoaded());

    Unique<Gui::Document> document = Gui::Document::Instantiate(*recipe->Get(), assets);
    REQUIRE(document != nullptr);

    HudModel model;
    model.player.health = 1.0f;

    Gui::BindingContext context;
    context.SetData(model);
    document->BindContext(&context, &Types);

    // An empty scene renders a cleared target the document composites over (a null World would
    // short-circuit Viewport::Render before the document blend).
    RegisterBuiltinTypes(Types);
    const Unique<Scene> scene = Scene::Create(Types);

    const Unique<Viewport> viewport = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = Extent},
        .ColorFormat = Format::RGBA16Sfloat,
        .Role = ViewportRole::Offscreen,
    });
    viewport->SetViewState({.World = scene.get(), .Delta = 0.016f});
    viewport->AttachDocument(*document);

    // The game resolves its bindings each frame; the engine then drives the attached document (Update
    // -> Solve at the region extent -> Build) and composites it. A full-health meter paints a wide
    // green fill.
    document->UpdateBindings();
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    const usize fullGreen = GreenPixels(viewport->GetOutput());
    CHECK(fullGreen > 0);

    // Move the bound model to half health and bump the context version; the next binding pass
    // re-resolves {player.health} and the fill shrinks, so fewer green pixels paint. This proves the
    // bind path drives the render, not just that some UI drew.
    model.player.health = 0.5f;
    context.Invalidate();
    document->UpdateBindings();
    Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    const usize halfGreen = GreenPixels(viewport->GetOutput());
    CHECK(halfGreen < fullGreen);
    CHECK(halfGreen > 0);

    document.reset();
    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ui document hud: cook-on-demand shadow-mount reloads behind a stable handle")
{
    // The editor panel's live recook cooks the source into an in-memory archive, shadow-mounts it,
    // and re-fetches the recipe behind its stable AssetId. Cook the fixture into two independent
    // archives at the same target id and prove the shadow-mount swap resolves the second behind the
    // stable id — the exact MountMemory hot-swap the panel rides, without the editor exe's CookSession.
    const path first = CookUiPack();
    const path second = CookUiPack();

    AssetManager assets(Context, Tasks, Types);

    // Mount the first archive on disk, load the document, and instantiate an initial tree.
    const auto baseMount = assets.Mount(first);
    REQUIRE(baseMount.has_value());
    AssetResult<AssetHandle<Gui::UIDocument>> handle =
        assets.LoadSync<Gui::UIDocument>(UIDocumentId);
    REQUIRE(handle.has_value());
    {
        const Unique<Gui::Document> before = Gui::Document::Instantiate(*handle->Get(), assets);
        REQUIRE(before != nullptr);
        CHECK(before->Root().Children.size() == 1);
    }

    // Read the second archive's bytes and shadow-mount them in memory over the on-disk mount — the
    // recook result the editor's RequestCook hands back. The prior handle is then dropped and the
    // cache collected so the stale entry evicts, exactly as the panel's cook continuation does;
    // the next resolve of the id then hits the shadow mount.
    std::ifstream in(second, std::ios::binary);
    REQUIRE(in.good());
    const vector<u8> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    {
        const MountHandle shadow = assets.MountMemory(bytes, "<recook>");
        handle = {};
        assets.CollectGarbage();

        // Re-fetch behind the stable id and re-instantiate: the recooked recipe resolves through the
        // shadow mount, and the document rebuilds behind the same handle id.
        handle = assets.LoadSync<Gui::UIDocument>(UIDocumentId);
        REQUIRE(handle.has_value());
        CHECK(handle->Id().Value == UIDocumentId.Value);
        const Unique<Gui::Document> after = Gui::Document::Instantiate(*handle->Get(), assets);
        REQUIRE(after != nullptr);
        CHECK(after->Root().Children.size() == 1);
    }

    // Drop the handle referencing the shadow-mounted recipe before the shadow unmounts, then collect
    // so the base mount's copy resolves cleanly afterward.
    handle = {};
    assets.CollectGarbage();

    // Dropping the shadow mount reveals the base mount again — the RAII unmount the panel gets when
    // it replaces the MountHandle on the next recook.
    const AssetResult<AssetHandle<Gui::UIDocument>> revealed =
        assets.LoadSync<Gui::UIDocument>(UIDocumentId);
    REQUIRE(revealed.has_value());

    // The on-disk base mount is still live on `assets` here, so the archive files are open;
    // Windows refuses to delete an open file (POSIX unlinks it). The scratch files are
    // transient OS-temp entries, so a failed cleanup is ignored rather than thrown.
    std::error_code ec;
    std::filesystem::remove(first, ec);
    std::filesystem::remove(second, ec);
}
