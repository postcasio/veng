// Shader load test (planset-5 plan 08): cooks the shader fixture pack in-process,
// mounts it, LoadSync<ShaderAsset>s it through AssetManager, and checks the
// loaded ShaderInterface — bindings, push constants, vertex inputs — plus the
// layout-builder helpers (BuildPushConstantRanges, BuildDescriptorSetLayouts,
// FindBinding, ValidateVertexLayout) against the canonical mesh vertex layout.

#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/Mesh.h>
#include <Veng/Renderer/ShaderAsset.h>

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

    AssetManager assets(Context);
    const VoidResult mountResult = assets.Mount(outArchive);
    REQUIRE(mountResult.has_value());

    const AssetResult<AssetHandle<ShaderAsset>> handle = assets.LoadSync<ShaderAsset>(AssetId{4001});
    REQUIRE(handle.has_value());
    REQUIRE(handle->IsLoaded());

    const ShaderAsset& asset = *handle->Get();
    REQUIRE(asset.Module != nullptr);

    const ShaderInterface& iface = asset.Interface;

    // mesh.vert.slang: no descriptor bindings (set 0 excluded — bindless
    // registry), one push constant, four vertex inputs.
    CHECK(iface.Bindings.empty());
    REQUIRE(iface.PushConstants.size() == 1);
    REQUIRE(iface.VertexInputs.GetElements().size() == 4);

    CHECK(iface.PushConstants[0].Name == "g_PushConstants");
    CHECK(iface.PushConstants[0].Offset == 0);
    CHECK(iface.PushConstants[0].Size == 128);
    CHECK(iface.PushConstants[0].Stages == ShaderStage::Vertex);

    // Vertex inputs in location order.
    const vector<VertexBufferElement>& verts = iface.VertexInputs.GetElements();
    CHECK(verts[0].Type == Format::RGB32Sfloat); // a_Position
    CHECK(verts[1].Type == Format::RGB32Sfloat); // a_Normal
    CHECK(verts[2].Type == Format::RGB32Sfloat); // a_Tangent
    CHECK(verts[3].Type == Format::RG32Sfloat);  // a_UV

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

    // ValidateVertexLayout: the mesh canonical layout matches what the shader
    // reflects — this must pass cleanly (no abort).
    const VertexBufferLayout canonical = Mesh::CanonicalLayout();
    iface.ValidateVertexLayout(canonical);

    std::filesystem::remove(outArchive);
}
