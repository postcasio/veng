// DocumentHost::SetOnInstantiate — the resolve-elements-once hook. Cooks the UI HUD fixture pack,
// mounts it, and drives the host's lazy load:
//  (a) a callback set before the first Attach fires exactly once, on the just-instantiated live
//      document, and a second Attach (which reuses the live tree) does not fire it again;
//  (b) a callback set on an already-live document fires immediately, so the ordering of the call
//      against Attach is a non-issue.

#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/DocumentHost.h>
#include <Veng/Renderer/Viewport.h>

#include <gpu/fixture.h>
#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The shared HUD fixture (tests/cooker/fixtures/ui_hud_pack.json); its UIDocument id, loadable
    // through the mounted archive.
    constexpr AssetId UIDocumentId{0xA09AA8B60AEAA8BEULL};
    constexpr uvec2 Extent{64, 48};

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
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "document host: SetOnInstantiate fires once on the lazy load")
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
    // Nothing is live yet, so the callback has not run.
    CHECK(calls == 0);

    const Unique<Viewport> viewport = MakeViewport(Context, assets);
    const Gui::Document* attached = host.Attach(*viewport);
    REQUIRE(attached != nullptr);

    // The lazy load instantiated and bound the document, then ran the callback exactly once on the
    // live tree.
    CHECK(calls == 1);
    CHECK(seen == attached);

    // A second attach reuses the live document (no re-instantiate), so the callback does not fire
    // again.
    host.Attach(*viewport);
    CHECK(calls == 1);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "document host: SetOnInstantiate fires immediately on an already-live document")
{
    const path archive = CookUiPack();

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    Gui::DocumentHost host(assets, Types, UIDocumentId);
    const Unique<Viewport> viewport = MakeViewport(Context, assets);
    const Gui::Document* attached = host.Attach(*viewport);
    REQUIRE(attached != nullptr);

    // Setting the callback after the document is already live runs it immediately, on that document.
    int calls = 0;
    Gui::Document* seen = nullptr;
    host.SetOnInstantiate(
        [&](Gui::Document& document)
        {
            ++calls;
            seen = &document;
        });
    CHECK(calls == 1);
    CHECK(seen == attached);

    std::filesystem::remove(archive);
}
