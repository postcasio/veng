// Vertex-layout load test: verifies the full cook→load
// round-trip for VertexLayout and confirms that AssetManager auto-mounts
// the embedded core pack (containing the engine's built-in layouts) at init.

#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/VertexLayout.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "vertex layout loader: cook fixture pack, mount, LoadSync")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "shader_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_gpu_vlayout.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    AssetManager assets(Context, Tasks, Types);
    const VoidResult mountResult = assets.Mount(outArchive);
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<VertexLayout>> handle =
        assets.LoadSync<VertexLayout>(AssetId{0x1B59});
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const vector<VertexBufferElement>& elems = handle->Get()->GetLayout().GetElements();
    REQUIRE(elems.size() == 4);
    CHECK(elems[0].Type == Format::RGB32Sfloat); // a_Position
    CHECK(elems[1].Type == Format::RGB32Sfloat); // a_Normal
    CHECK(elems[2].Type == Format::RGB32Sfloat); // a_Tangent
    CHECK(elems[3].Type == Format::RG32Sfloat);  // a_UV

    std::filesystem::remove(outArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "vertex layout loader: core pack auto-mounted, canonical layout loadable")
{
    // A freshly-constructed AssetManager auto-mounts the embedded core pack.
    // The canonical layout (position/normal/tangent/uv) must be loadable by
    // Mesh::CanonicalLayoutId without any explicit mount call. Tangent is a
    // vec4 (RGBA32Sfloat): xyz tangent + w bitangent handedness.
    AssetManager assets(Context, Tasks, Types);

    const AssetResult<AssetHandle<VertexLayout>> handle =
        assets.LoadSync<VertexLayout>(Mesh::CanonicalLayoutId);
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const vector<VertexBufferElement>& elems = handle->Get()->GetLayout().GetElements();
    REQUIRE(elems.size() == 4);
    CHECK(elems[0].Type == Format::RGB32Sfloat);  // a_Position
    CHECK(elems[1].Type == Format::RGB32Sfloat);  // a_Normal
    CHECK(elems[2].Type == Format::RGBA32Sfloat); // a_Tangent (xyz + handedness w)
    CHECK(elems[3].Type == Format::RG32Sfloat);   // a_UV
}
