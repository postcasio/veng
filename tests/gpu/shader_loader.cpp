// Shader load test: cooks the shader fixture pack in-process,
// mounts it, LoadSync<Shader>s it through AssetManager, and checks the
// loaded ShaderInterface — bindings, push constants, VertexLayoutId — plus the
// layout-builder helpers (BuildPushConstantRanges, BuildDescriptorSetLayouts,
// FindBinding). Also verifies that the referenced VertexLayout (id 7001)
// loads and carries the expected 4-element canonical layout.

#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Asset/VertexLayout.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "shader loader: cook, mount, LoadSync, validate interface")
{
    const path fixtureDir = path(GPU_COOKER_FIXTURE_DIR);
    const path packJson = fixtureDir / "shader_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_gpu_shader.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    AssetManager assets(Context, Tasks);
    const VoidResult mountResult = assets.Mount(outArchive);
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<Shader>> handle = assets.LoadSync<Shader>(AssetId{4001});
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const Shader& asset = *handle->Get();
    REQUIRE(asset.Module != nullptr);

    const ShaderInterface& iface = asset.Interface;

    // mesh.vert.slang: no descriptor bindings (set 0 excluded — bindless
    // registry), one push constant, vertex layout referenced by AssetId 7001.
    CHECK(iface.Bindings.empty());
    REQUIRE(iface.PushConstants.size() == 1);
    REQUIRE(iface.VertexLayoutId.has_value());
    CHECK(*iface.VertexLayoutId == AssetId{7001});

    CHECK(iface.PushConstants[0].Name == "g_PushConstants");
    CHECK(iface.PushConstants[0].Offset == 0);
    CHECK(iface.PushConstants[0].Size == 128);
    CHECK(iface.PushConstants[0].Stages == ShaderStage::Vertex);

    // FindBinding: push constants are not bindings; descriptor lookup misses.
    CHECK(!iface.FindBinding("g_PushConstants").has_value());
    CHECK(!iface.FindBinding("nonexistent").has_value());

    // BuildPushConstantRanges: one range matching the push constant block.
    const vector<PushConstantRange> ranges = iface.BuildPushConstantRanges();
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].Stages == ShaderStage::Vertex);
    CHECK(ranges[0].Offset == 0);
    CHECK(ranges[0].Size == 128);

    // BuildDescriptorSetLayouts: no author bindings (set 0 is the registry's)
    // → empty vector.
    const vector<Ref<DescriptorSetLayout>> layouts = iface.BuildDescriptorSetLayouts(Context, "ShaderTest");
    CHECK(layouts.empty());

    // Load the referenced VertexLayout and verify its 4-element layout.
    const AssetResult<AssetHandle<VertexLayout>> layoutHandle =
        assets.LoadSync<VertexLayout>(AssetId{7001});
    REQUIRE(layoutHandle.has_value());
    REQUIRE(layoutHandle->IsLoaded());

    const vector<VertexBufferElement>& elems = layoutHandle->Get()->GetLayout().GetElements();
    REQUIRE(elems.size() == 4);
    CHECK(elems[0].Type == Format::RGB32Sfloat); // a_Position
    CHECK(elems[1].Type == Format::RGB32Sfloat); // a_Normal
    CHECK(elems[2].Type == Format::RGB32Sfloat); // a_Tangent
    CHECK(elems[3].Type == Format::RG32Sfloat);  // a_UV

    std::filesystem::remove(outArchive);
}
