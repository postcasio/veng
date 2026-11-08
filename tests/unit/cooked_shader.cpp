// The cooked shader blob round-trips through the writer and the parser the runtime loader uses,
// with one section per compiled target, and a missing target is reported by the asset's name.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include <Veng/Asset/CookedShader.h>

using namespace Veng;

namespace
{
    CookedDescriptorBinding MakeBinding()
    {
        CookedDescriptorBinding binding{
            .Set = 3,
            .Binding = 2,
            .Type = 1,
            .Count = 4,
            .StageMask = 2,
            .MetalBinding = static_cast<u32>(CookedMetalBinding::ArgumentBufferElement),
            .MetalIndex = 5,
        };
        std::memcpy(binding.Name, "Many", 5);
        return binding;
    }
}

TEST_CASE("CookedShader: a blob carrying SPIR-V and a Metal library round-trips both")
{
    const std::array<u8, 8> spirv{1, 2, 3, 4, 5, 6, 7, 8};
    const std::array<u8, 3> metal{9, 10, 11};
    const std::array bindings{MakeBinding()};
    const std::array sections{
        CookedShaderSectionInput{
            .Code = CookedShaderCode::SpirV, .Bytes = spirv, .EntryPoint = "main"},
        CookedShaderSectionInput{
            .Code = CookedShaderCode::MetalLibrary, .Bytes = metal, .EntryPoint = "main_0"},
    };

    const vector<u8> blob = BuildCookedShaderBlob(
        CookedShaderInterfaceHeader{.VertexLayoutAssetId = 42}, bindings, {}, sections);
    const Result<CookedShaderView> view = ParseCookedShader(blob);
    REQUIRE_MESSAGE(view.has_value(), view.error());

    CHECK(view->Interface.VertexLayoutAssetId == 42);
    REQUIRE(view->Bindings.size() == 1);
    CHECK(view->Bindings[0].MetalIndex == 5);
    CHECK(std::string_view(view->Bindings[0].Name) == "Many");
    CHECK(view->PushConstants.empty());

    const Result<const CookedShaderSectionView*> foundSpirv =
        FindCookedShaderSection(*view, CookedShaderCode::SpirV, "test");
    REQUIRE(foundSpirv.has_value());
    CHECK(std::ranges::equal((*foundSpirv)->Bytes, spirv));
    CHECK((*foundSpirv)->EntryPoint == "main");

    const Result<const CookedShaderSectionView*> foundMetal =
        FindCookedShaderSection(*view, CookedShaderCode::MetalLibrary, "test");
    REQUIRE(foundMetal.has_value());
    CHECK(std::ranges::equal((*foundMetal)->Bytes, metal));
    CHECK((*foundMetal)->EntryPoint == "main_0");
}

TEST_CASE("CookedShader: a SPIR-V-only blob reports its missing Metal section by the asset's name")
{
    const std::array<u8, 4> spirv{1, 2, 3, 4};
    const std::array sections{CookedShaderSectionInput{
        .Code = CookedShaderCode::SpirV, .Bytes = spirv, .EntryPoint = "main"}};
    const vector<u8> blob = BuildCookedShaderBlob({}, {}, {}, sections);
    const Result<CookedShaderView> view = ParseCookedShader(blob);
    REQUIRE_MESSAGE(view.has_value(), view.error());

    CHECK(FindCookedShaderSection(*view, CookedShaderCode::SpirV, "lit_surface").has_value());
    const Result<const CookedShaderSectionView*> missing =
        FindCookedShaderSection(*view, CookedShaderCode::MetalLibrary, "lit_surface");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().find("lit_surface") != string::npos);
    CHECK(missing.error().find("Metal library") != string::npos);
}

TEST_CASE("CookedShader: a blob of another version or a truncated one is rejected")
{
    const std::array<u8, 4> spirv{1, 2, 3, 4};
    const std::array sections{CookedShaderSectionInput{
        .Code = CookedShaderCode::SpirV, .Bytes = spirv, .EntryPoint = "main"}};
    vector<u8> blob = BuildCookedShaderBlob({}, {}, {}, sections);

    const vector<u8> truncated(blob.begin(), blob.end() - 2);
    CHECK_FALSE(ParseCookedShader(truncated).has_value());

    blob[0] = static_cast<u8>(CookedShaderVersion + 1);
    CHECK_FALSE(ParseCookedShader(blob).has_value());
}
