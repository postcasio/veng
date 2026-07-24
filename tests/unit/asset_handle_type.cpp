// The two halves of AssetHandle<T> field resolution, both pure: the builtin leaf → asset-type
// mapping RegisterBuiltinAssetTypes installs (the prefab loader, the cooker's two validation
// hooks, and the editor's picker all share it), and AssetHandleFieldAccepts, the one substitution
// rule the engine allows between a field's expected type and a reference's actual type.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetHandleType.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Gui/StyleSheet.h>
#include <Veng/Gui/UIDocument.h>
#include <Veng/Reflection/TypeId.h>

using namespace Veng;

TEST_CASE("AssetHandle leaves: every builtin handle leaf resolves to its asset type")
{
    AssetTypeRegistry types;
    RegisterBuiltinAssetTypes(types);

    // The leaves that predate the registry.
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<RawAsset>>()) == AssetTypes::Raw);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<Texture>>()) == AssetTypes::Texture);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<Mesh>>()) == AssetTypes::Mesh);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<Material>>()) == AssetTypes::Material);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<MaterialInstance>>()) ==
          AssetTypes::MaterialInstance);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<Prefab>>()) == AssetTypes::Prefab);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<Animation>>()) == AssetTypes::Animation);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<EnvironmentMap>>()) ==
          AssetTypes::Environment);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<InputMappingContext>>()) ==
          AssetTypes::InputMap);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<Gui::UIDocument>>()) ==
          AssetTypes::UIDocument);

    // The content types that gained a leaf so a component can reference them.
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<Level>>()) == AssetTypes::Level);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<Skeleton>>()) == AssetTypes::Skeleton);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<Font>>()) == AssetTypes::Font);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<Gui::StyleSheet>>()) ==
          AssetTypes::StyleSheet);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<TableSchema>>()) == AssetTypes::TableSchema);
    CHECK(types.FindByHandleField(TypeIdOf<AssetHandle<DataTable>>()) == AssetTypes::DataTable);
}

TEST_CASE("AssetHandle leaves: the material-pipeline types deliberately have none")
{
    AssetTypeRegistry types;
    RegisterBuiltinAssetTypes(types);

    // A Shader and a VertexLayout are wiring inside the material system — nothing outside it can
    // consume one, so neither is referenceable from a component and neither claims a leaf.
    const AssetTypeInfo* shader = types.Find(AssetTypes::Shader);
    REQUIRE(shader != nullptr);
    CHECK(shader->HandleFieldType == 0);

    const AssetTypeInfo* layout = types.Find(AssetTypes::VertexLayout);
    REQUIRE(layout != nullptr);
    CHECK(layout->HandleFieldType == 0);

    // A leaf that is not an AssetHandle at all resolves to nothing, as does an unclaimed id.
    CHECK_FALSE(types.FindByHandleField(TypeIdOf<f32>()).has_value());
    CHECK_FALSE(types.FindByHandleField(0xFFFFFFFFFFFFFFFFULL).has_value());
}

TEST_CASE("AssetHandle leaves: a registered leaf id is unique across the builtins")
{
    AssetTypeRegistry types;
    RegisterBuiltinAssetTypes(types);

    // Register aborts on a duplicate leaf id, so reaching here already proves uniqueness; this
    // pins the count so a type registered with a copy-pasted leaf id is caught as a lost entry
    // rather than passing silently.
    usize withLeaf = 0;
    for (const auto& [id, info] : types.All())
    {
        if (info.HandleFieldType != 0)
        {
            ++withLeaf;
            CHECK(types.FindByHandleField(info.HandleFieldType) == id);
        }
    }
    CHECK(withLeaf == 17);
}

TEST_CASE("AssetHandleFieldAccepts: a reference of the field's own type is accepted")
{
    CHECK(AssetHandleFieldAccepts(AssetTypes::Texture, AssetTypes::Texture));
    CHECK(AssetHandleFieldAccepts(AssetTypes::Material, AssetTypes::Material));
    CHECK(AssetHandleFieldAccepts(AssetTypes::MaterialInstance, AssetTypes::MaterialInstance));
    CHECK(AssetHandleFieldAccepts(AssetTypes::DataTable, AssetTypes::DataTable));

    // Identity holds for a type the engine never heard of, so a game type needs no registration
    // for its own references to validate.
    constexpr AssetTypeId custom{0x0123456789ABCDEFULL};
    CHECK(AssetHandleFieldAccepts(custom, custom));
}

TEST_CASE("AssetHandleFieldAccepts: a MaterialInstance field accepts a bare Material")
{
    // The engine's one substitution: the load resolves the material to its zero-override default
    // instance, so authoring the material id directly is legal.
    CHECK(AssetHandleFieldAccepts(AssetTypes::MaterialInstance, AssetTypes::Material));
}

TEST_CASE("AssetHandleFieldAccepts: the substitution does not run the other way")
{
    // A Material field given a MaterialInstance is a genuine mismatch — an instance is not a
    // material, and nothing resolves it back to one.
    CHECK_FALSE(AssetHandleFieldAccepts(AssetTypes::Material, AssetTypes::MaterialInstance));
}

TEST_CASE("AssetHandleFieldAccepts: any other pairing is rejected")
{
    CHECK_FALSE(AssetHandleFieldAccepts(AssetTypes::Mesh, AssetTypes::Texture));
    CHECK_FALSE(AssetHandleFieldAccepts(AssetTypes::DataTable, AssetTypes::TableSchema));
    CHECK_FALSE(AssetHandleFieldAccepts(AssetTypes::TableSchema, AssetTypes::DataTable));
    CHECK_FALSE(AssetHandleFieldAccepts(AssetTypes::MaterialInstance, AssetTypes::Texture));

    // The substitution is specific to the pair, not a general "anything for a MaterialInstance"
    // or "a Material for anything".
    CHECK_FALSE(AssetHandleFieldAccepts(AssetTypes::Texture, AssetTypes::Material));

    // The invalid id is a type like any other: it matches only itself.
    CHECK_FALSE(AssetHandleFieldAccepts(AssetTypes::Texture, AssetTypeId{}));
}
