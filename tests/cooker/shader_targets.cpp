// Shader targets: which targets a build cooks, the Metal binding model each shape of binding cooks
// to, the cross-target layout check, and the SPIR-V NonUniform decoration surviving the portable
// index wrapper. The Metal cases skip where the host has no Metal toolchain.

#include <algorithm>
#include <cstring>
#include <string_view>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedShader.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/MetalBindingSlots.h>
#include <Veng/Cook/ShaderTargets.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr AssetId ShaderId{0x5EC7000000000001ULL};

    Result<vector<u8>> CookShader(const std::string_view name, const ShaderTargetSet targets)
    {
        Cooker cooker;
        cooker.SetShaderTargets(targets);
        RegisterBuiltinImporters(cooker);
        const path source = path(VENG_COOKER_TEST_FIXTURE_DIR) / "shaders" / name;
        return cooker.CookSource(source, ShaderId, AssetTypes::Shader, {}, nullptr, nullptr,
                                 nullptr, path(VENG_CORE_SHADER_DIR));
    }

    // The cooked shader inside a single-entry archive; its sections view into the archive.
    CookedShaderView ParseArchived(const Result<ArchiveReader>& reader)
    {
        REQUIRE(reader.has_value());
        const optional<ArchiveEntry> entry = reader->Find(ShaderId);
        REQUIRE(entry.has_value());
        const Result<CookedShaderView> shader = ParseCookedShader(entry->Blob);
        REQUIRE_MESSAGE(shader.has_value(), shader.error());
        return *shader;
    }

    const CookedDescriptorBinding* FindBinding(const CookedShaderView& shader,
                                               const std::string_view name)
    {
        const auto it = std::ranges::find_if(shader.Bindings, [&](const CookedDescriptorBinding& b)
                                             { return std::string_view(b.Name) == name; });
        return it != shader.Bindings.end() ? &*it : nullptr;
    }

    bool HasMetalToolchain()
    {
        if (!ProbeMetalToolchain())
        {
            MESSAGE("no Metal toolchain (xcrun metal) on this host; the Metal case is skipped");
            return false;
        }
        return true;
    }

    constexpr ShaderTargetSet SpirvOnly = ShaderTargetSet{}.With(ShaderTarget::SpirV);
    constexpr ShaderTargetSet Both =
        ShaderTargetSet{}.With(ShaderTarget::SpirV).With(ShaderTarget::Metal);
}

TEST_CASE("ShaderTargets: Native follows the backend, All selects SPIR-V and Metal")
{
    CHECK(ResolveShaderTargets("Native", "Vulkan").value_or(ShaderTargetSet{}) == SpirvOnly);
    CHECK(ResolveShaderTargets("Native", "Metal").value_or(ShaderTargetSet{}) ==
          ShaderTargetSet{}.With(ShaderTarget::Metal));
    CHECK(ResolveShaderTargets("All", "Vulkan").value_or(ShaderTargetSet{}) == Both);
    CHECK(ResolveShaderTargets("spirv, metal", "Vulkan").value_or(ShaderTargetSet{}) == Both);
    CHECK_FALSE(ResolveShaderTargets("Everything", "Vulkan").has_value());
    CHECK_FALSE(ResolveShaderTargets("Native", "Direct3D").has_value());
}

TEST_CASE("ShaderTargets: a SPIR-V cook stores one SPIR-V section and no Metal placement")
{
    const Result<vector<u8>> bytes = CookShader("metal_model.comp.shader.json", SpirvOnly);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error());
    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(*bytes);
    const CookedShaderView shader = ParseArchived(reader);

    REQUIRE(shader.Sections.size() == 1);
    CHECK(shader.Sections[0].Code == CookedShaderCode::SpirV);
    for (const CookedDescriptorBinding& binding : shader.Bindings)
    {
        CHECK(binding.MetalBinding == static_cast<u32>(CookedMetalBinding::None));
    }
}

TEST_CASE("ShaderTargets: the SPIR-V NonUniform decoration survives NonUniformIndex")
{
    const Result<vector<u8>> bytes = CookShader("metal_model.comp.shader.json", SpirvOnly);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error());
    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(*bytes);
    const CookedShaderView shader = ParseArchived(reader);
    const std::span<const u8> spirv = shader.Sections[0].Bytes;
    REQUIRE(spirv.size() % 4 == 0);

    // Walk the instruction stream past the five-word header: OpDecorate (71) with the NonUniform
    // decoration (5300).
    constexpr u32 OpDecorate = 71;
    constexpr u32 DecorationNonUniform = 5300;
    vector<u32> words(spirv.size() / 4);
    std::memcpy(words.data(), spirv.data(), spirv.size());
    bool decorated = false;
    for (usize i = 5; i < words.size();)
    {
        const u32 count = words[i] >> 16;
        const u32 opcode = words[i] & 0xFFFFu;
        if (count == 0)
        {
            break;
        }
        if (opcode == OpDecorate && count >= 3 && words[i + 2] == DecorationNonUniform)
        {
            decorated = true;
        }
        i += count;
    }
    CHECK(decorated);
}

TEST_CASE("ShaderTargets: each binding shape takes its Metal element or slot")
{
    if (!HasMetalToolchain())
    {
        return;
    }
    const Result<vector<u8>> bytes = CookShader("metal_model.comp.shader.json", Both);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error());
    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(*bytes);
    const CookedShaderView shader = ParseArchived(reader);

    REQUIRE(shader.Sections.size() == 2);
    const Result<const CookedShaderSectionView*> library =
        FindCookedShaderSection(shader, CookedShaderCode::MetalLibrary, "metal_model");
    REQUIRE(library.has_value());
    CHECK_FALSE((*library)->Bytes.empty());
    CHECK_FALSE((*library)->EntryPoint.empty());

    // The set's fields take consecutive SPIR-V bindings and consecutive argument-buffer elements:
    // a combined image sampler takes two elements, an array one per element.
    const auto element = [&](const std::string_view name, const u32 binding, const u32 index)
    {
        const CookedDescriptorBinding* found = FindBinding(shader, name);
        REQUIRE_MESSAGE(found != nullptr, name);
        CHECK(found->Set == 3);
        CHECK(found->Binding == binding);
        CHECK(found->MetalBinding == static_cast<u32>(CookedMetalBinding::ArgumentBufferElement));
        CHECK(found->MetalIndex == index);
    };
    element("ShadowMap", 0, 0);
    element("Compare", 1, 1);
    element("Output", 2, 2);
    element("Volume", 3, 3);
    element("Combined", 4, 4);
    element("Many", 5, 6);
    element("Data", 6, 10);
    CHECK(FindBinding(shader, "Many")->Count == 4);

    // A dynamic uniform buffer stays outside the block, at the first reserved dynamic slot.
    const CookedDescriptorBinding* constants = FindBinding(shader, "g_Constants");
    REQUIRE(constants != nullptr);
    CHECK(constants->Binding == 7);
    CHECK(constants->MetalBinding == static_cast<u32>(CookedMetalBinding::BufferSlot));
    CHECK(constants->MetalIndex == MetalBindingSlots::DynamicUniformBufferBase);

    // The bindless sets reach Metal too, and stay out of the declared interface.
    for (const CookedDescriptorBinding& binding : shader.Bindings)
    {
        CHECK(binding.Set >= 3);
    }
    REQUIRE(shader.PushConstants.size() == 1);
}

TEST_CASE("ShaderTargets: vertex inputs and the instance index compile for Metal")
{
    if (!HasMetalToolchain())
    {
        return;
    }
    const Result<vector<u8>> bytes = CookShader("metal_vertex.vert.shader.json", Both);
    REQUIRE_MESSAGE(bytes.has_value(), bytes.error());
    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(*bytes);
    const CookedShaderView shader = ParseArchived(reader);
    CHECK(FindCookedShaderSection(shader, CookedShaderCode::MetalLibrary, "metal_vertex"));
}

TEST_CASE("ShaderTargets: a layout the two targets disagree on fails the cook, naming the member")
{
    if (!HasMetalToolchain())
    {
        return;
    }
    const Result<vector<u8>> block = CookShader("layout_block_mismatch.comp.shader.json", Both);
    REQUIRE_FALSE(block.has_value());
    CHECK(block.error().find("'Light' member 'Scale'") != string::npos);

    const Result<vector<u8>> buffer = CookShader("layout_buffer_mismatch.comp.shader.json", Both);
    REQUIRE_FALSE(buffer.has_value());
    CHECK(buffer.error().find("'Point' member 'Position'") != string::npos);

    const Result<vector<u8>> agreed = CookShader("layout_agreed.comp.shader.json", Both);
    CHECK_MESSAGE(agreed.has_value(), agreed.error());
}
