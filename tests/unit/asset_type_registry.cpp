// AssetTypeRegistry: the name ↔ id ↔ display mapping a pack manifest and the editor
// resolve through. Pure, no GPU.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/HexId.h>

using namespace Veng;

TEST_CASE("AssetTypeRegistry: the builtins round-trip name -> id -> name")
{
    AssetTypeRegistry types;
    RegisterBuiltinAssetTypes(types);

    CHECK(types.All().size() == 20);

    for (const auto& [id, info] : types.All())
    {
        const optional<AssetTypeId> resolved = types.FindByName(info.Name);
        REQUIRE(resolved.has_value());
        CHECK(*resolved == id);
        CHECK(types.GetName(*resolved) == info.Name);
        CHECK(id.IsValid());
    }

    // The manifest names the cooker and the editor's source index agree on.
    CHECK(types.FindByName("Texture") == optional<AssetTypeId>{AssetTypes::Texture});
    CHECK(types.FindByName("MaterialInstance") ==
          optional<AssetTypeId>{AssetTypes::MaterialInstance});
    CHECK(types.IsRegistered(AssetTypes::Prefab));
}

TEST_CASE("AssetTypeRegistry: an unregistered name resolves to nothing")
{
    AssetTypeRegistry types;
    RegisterBuiltinAssetTypes(types);

    CHECK_FALSE(types.FindByName("NoSuchType").has_value());
    CHECK_FALSE(types.FindByName("").has_value());
    // Names are exact: the canonical spelling is the only one that resolves.
    CHECK_FALSE(types.FindByName("texture").has_value());
}

TEST_CASE("AssetTypeRegistry: an unknown id degrades to its hex spelling, never a crash")
{
    AssetTypeRegistry types;
    RegisterBuiltinAssetTypes(types);

    const AssetTypeId unknown{0xABCDEF0123456789ULL};
    CHECK_FALSE(types.IsRegistered(unknown));
    CHECK(types.Find(unknown) == nullptr);
    CHECK(types.GetName(unknown) == FormatHexId(unknown.Value));
    CHECK(types.GetDisplayName(unknown) == FormatHexId(unknown.Value));
    CHECK(types.GetGlyph(unknown) == "?");

    // The invalid id is unknown like any other, not a special case.
    CHECK_FALSE(types.IsRegistered(AssetTypeId{}));
    CHECK(types.GetGlyph(AssetTypeId{}) == "?");
}

TEST_CASE("AssetTypeRegistry: a game-registered type resolves beside the builtins")
{
    AssetTypeRegistry types;
    RegisterBuiltinAssetTypes(types);

    const AssetTypeId custom{0x0123456789ABCDEFULL};
    types.Register(
        {.Id = custom, .Name = "SpriteAtlas", .DisplayName = "Sprite Atlas", .Glyph = "ATL"});

    CHECK(types.FindByName("SpriteAtlas") == optional<AssetTypeId>{custom});
    CHECK(types.GetDisplayName(custom) == "Sprite Atlas");
    CHECK(types.GetGlyph(custom) == "ATL");
    // Registering it did not disturb the builtins.
    CHECK(types.FindByName("Texture") == optional<AssetTypeId>{AssetTypes::Texture});
}

TEST_CASE("AssetTypeRegistry: an empty display name falls back to the canonical name")
{
    AssetTypeRegistry types;
    const AssetTypeId custom{0x00000000DEADBEEFULL};
    types.Register({.Id = custom, .Name = "Bare"});

    CHECK(types.GetDisplayName(custom) == "Bare");
    CHECK(types.GetGlyph(custom) == "?");
}
