// ShaderInterface unit cases. FindBinding, BuildPushConstantRanges, and
// GroupBindingsBySet are pure data->data transforms — no Context, no device — so
// the lookup, the push-constant mapping, and the descriptor-set grouping that
// BuildDescriptorSetLayouts builds on are testable without a GPU. The set-numbering
// asserts GroupBindingsBySet enforces (set 0 reserved, contiguous sets) are pinned
// by the death band.

#include <doctest/doctest.h>

#include <Veng/Renderer/ShaderInterface.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    ShaderBinding Binding(string name, u32 set, u32 binding,
                          DescriptorType type = DescriptorType::UniformBuffer)
    {
        return {.Name = std::move(name),
                .Set = set,
                .Binding = binding,
                .Type = type,
                .Count = 1,
                .Stages = ShaderStage::Fragment};
    }
}

TEST_CASE("ShaderInterface::FindBinding returns the named binding or nullopt")
{
    ShaderInterface iface;
    iface.Bindings = {Binding("Albedo", 1, 0), Binding("Params", 1, 1)};

    const auto albedo = iface.FindBinding("Albedo");
    REQUIRE(albedo.has_value());
    CHECK(albedo->Set == 1);
    CHECK(albedo->Binding == 0);

    CHECK_FALSE(iface.FindBinding("Missing").has_value());
}

TEST_CASE("ShaderInterface::BuildPushConstantRanges maps each block to a range")
{
    ShaderInterface iface;
    iface.PushConstants = {
        {.Name = "Push", .Offset = 0, .Size = 64, .Stages = ShaderStage::Vertex},
        {.Name = "More", .Offset = 64, .Size = 16, .Stages = ShaderStage::Fragment},
    };

    const vector<PushConstantRange> ranges = iface.BuildPushConstantRanges();

    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0].Offset == 0);
    CHECK(ranges[0].Size == 64);
    CHECK(ranges[0].Stages == ShaderStage::Vertex);
    CHECK(ranges[1].Offset == 64);
    CHECK(ranges[1].Size == 16);
    CHECK(ranges[1].Stages == ShaderStage::Fragment);
}

TEST_CASE("ShaderInterface::GroupBindingsBySet partitions bindings per set in set order")
{
    ShaderInterface iface;
    // Two bindings in set 1, one in set 2 — declared out of set order to prove the
    // grouping keys on Set, not declaration order.
    iface.Bindings = {
        Binding("A", 1, 0, DescriptorType::UniformBuffer),
        Binding("C", 2, 0, DescriptorType::StorageImage),
        Binding("B", 1, 1, DescriptorType::CombinedImageSampler),
    };

    const vector<vector<DescriptorBinding>> bySet = iface.GroupBindingsBySet();

    // Element i holds set (i + 1)'s bindings.
    REQUIRE(bySet.size() == 2);
    REQUIRE(bySet[0].size() == 2);
    REQUIRE(bySet[1].size() == 1);

    // Set 1: the two bindings, in declaration order, with their fields carried over.
    CHECK(bySet[0][0].Binding == 0);
    CHECK(bySet[0][0].Type == DescriptorType::UniformBuffer);
    CHECK(bySet[0][1].Binding == 1);
    CHECK(bySet[0][1].Type == DescriptorType::CombinedImageSampler);

    // Set 2: the single storage-image binding.
    CHECK(bySet[1][0].Binding == 0);
    CHECK(bySet[1][0].Type == DescriptorType::StorageImage);
}

TEST_CASE("ShaderInterface::GroupBindingsBySet returns empty for an interface with no bindings")
{
    const ShaderInterface iface;
    CHECK(iface.GroupBindingsBySet().empty());
}
