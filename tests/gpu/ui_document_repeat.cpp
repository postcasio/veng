// UI-document repetition runtime test: cooks a document whose single classed child carries a
// `count="4"` with `${n}` substitution into its id and text, LoadSyncs and Instantiates it, and
// asserts Document::FindAllByClass resolves the unrolled pool — four elements in depth-first tree
// order, each carrying its substituted id and text. The cook-time unroll is invisible to the
// runtime (the cooked format is unchanged), so the pool resolves exactly as four hand-authored
// siblings would.

#include <filesystem>
#include <fstream>
#include "support/TempPath.h"

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/UIDocument.h>

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    constexpr AssetId PoolDocumentId{0x2D5E729A12F26978ULL};
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ui document repeat: FindAllByClass resolves a cooked count pool in tree order")
{
    const path dir = Veng::TestSupport::TempDir();
    const path sourcePath = dir / "veng_gpu_pool.vui.xml";
    const path packPath = dir / "veng_gpu_pool.pack.json";
    const path outArchive = dir / "veng_gpu_pool.vengpack";

    std::ofstream(sourcePath) << R"(<Panel>
  <Text class="cell" id="cell-${n}" count="4">Cell ${n}</Text>
</Panel>)";
    std::ofstream(packPath) << R"({
  "version": 1,
  "assets": [
    { "id": "0x2D5E729A12F26978", "type": "UIDocument", "source": "veng_gpu_pool.vui.xml" }
  ]
})";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packPath, outArchive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(outArchive).has_value());

    const AssetResult<AssetHandle<Gui::UIDocument>> handle =
        assets.LoadSync<Gui::UIDocument>(PoolDocumentId);
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    // The cooked recipe is a Panel root with four unrolled Text children — indistinguishable from
    // four hand-authored siblings.
    const Gui::UIDocument& recipe = *handle->Get();
    REQUIRE(recipe.GetElements().size() == 5);

    const Unique<Gui::Document> document = Gui::Document::Instantiate(recipe, assets);
    REQUIRE(document != nullptr);
    REQUIRE(document->Root().Children.size() == 4);

    // FindAllByClass walks the pool in tree order; each element carries its substituted id + text.
    const vector<Gui::Element*> cells = document->FindAllByClass("cell");
    REQUIRE(cells.size() == 4);
    for (usize index = 0; index < cells.size(); ++index)
    {
        CHECK(cells[index] == document->Root().Children[index]);
        CHECK(cells[index]->Kind == Gui::ElementKind::Text);
        CHECK(cells[index]->Id == fmt::format("cell-{}", index + 1));
        CHECK(cells[index]->Text == fmt::format("Cell {}", index + 1));
    }

    // The const overload resolves the same pool.
    const Gui::Document& constDocument = *document;
    const vector<const Gui::Element*> constCells = constDocument.FindAllByClass("cell");
    CHECK(constCells.size() == 4);

    // An unmatched class resolves to an empty vector.
    CHECK(document->FindAllByClass("missing").empty());

    std::filesystem::remove(outArchive);
}
