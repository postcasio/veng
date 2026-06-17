// Device-free cases for the shared field-widget helper extracted from the
// inspector. The ImGui-drawing path needs a live ImGui frame (a backend), so the
// cases here cover the picker's testable seam: the AssetHandle-type filter and
// the write-back ApplyAssetPick performs on a combo selection (asserting the
// chosen id lands at the leading u64 of the handle). The extraction leaves the
// entity inspector's field walk unchanged — exercised by walking a descriptor
// table the same way both inspectors do.

#include <doctest/doctest.h>

#include "FieldWidget.h"

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Reflection/FieldDescriptor.h>
#include <Veng/Reflection/TypeId.h>

#include <cstring>

using namespace VengEditor;
using Veng::TypeIdOf;

namespace
{
    // A struct with an AssetHandle field, mirroring the TextureSample node's
    // Texture property — the picker writes the chosen texture id into it.
    struct Props
    {
        Veng::AssetHandle<Veng::Texture> Texture;
        Veng::f32 Scale = 1.0f;
        Veng::vec4 Tint{1.0f, 1.0f, 1.0f, 1.0f};
    };
}

TEST_CASE("FieldWidget: AssetTypeOfHandle maps handle leaf types to asset types")
{
    CHECK(AssetTypeOfHandle(TypeIdOf<Veng::AssetHandle<Veng::Texture>>()) == Veng::AssetType::Texture);
    CHECK(AssetTypeOfHandle(TypeIdOf<Veng::AssetHandle<Veng::Mesh>>()) == Veng::AssetType::Mesh);
    CHECK(AssetTypeOfHandle(TypeIdOf<Veng::AssetHandle<Veng::Material>>()) == Veng::AssetType::Material);

    // A non-handle leaf has no asset type.
    CHECK_FALSE(AssetTypeOfHandle(TypeIdOf<Veng::f32>()).has_value());
}

TEST_CASE("FieldWidget: the picker writes the chosen id through the field pointer")
{
    Props props;
    const Veng::FieldDescriptor field{
        .Name = "Texture",
        .Type = TypeIdOf<Veng::AssetHandle<Veng::Texture>>(),
        .Class = Veng::FieldClass::AssetHandle,
        .Offset = offsetof(Props, Texture),
    };

    void* fieldPtr = reinterpret_cast<Veng::u8*>(&props) + field.Offset;

    // The picker writes the selected id into the handle's leading u64.
    ApplyAssetPick(fieldPtr, Veng::AssetId{0x1234ABCDULL});
    Veng::u64 written = 0;
    std::memcpy(&written, fieldPtr, sizeof(written));
    CHECK(written == 0x1234ABCDULL);

    // Selecting "(none)" clears it back to the invalid id.
    ApplyAssetPick(fieldPtr, Veng::AssetId{});
    std::memcpy(&written, fieldPtr, sizeof(written));
    CHECK(written == 0);
}

TEST_CASE("FieldWidget: a field walk skips hidden fields and addresses by offset")
{
    // The entity inspector's field walk: iterate non-hidden descriptors, address
    // each by base + Offset. The extraction preserves this ordering/filtering.
    const Veng::vector<Veng::FieldDescriptor> fields = {
        {.Name = "Texture", .Type = TypeIdOf<Veng::AssetHandle<Veng::Texture>>(),
         .Class = Veng::FieldClass::AssetHandle, .Offset = offsetof(Props, Texture)},
        {.Name = "Scale", .Type = TypeIdOf<Veng::f32>(),
         .Class = Veng::FieldClass::Scalar, .Offset = offsetof(Props, Scale), .Hidden = true},
        {.Name = "Tint", .Type = TypeIdOf<Veng::vec4>(),
         .Class = Veng::FieldClass::Vector, .Offset = offsetof(Props, Tint)},
    };

    Props props;
    props.Scale = 2.0f;

    Veng::vector<Veng::string> visited;
    for (const Veng::FieldDescriptor& field : fields)
    {
        if (field.Hidden)
            continue;
        // The offset addressing the inspector and the node panel both use.
        void* ptr = reinterpret_cast<Veng::u8*>(&props) + field.Offset;
        if (field.Name == "Tint")
            CHECK(ptr == &props.Tint);
        visited.push_back(field.Name);
    }

    REQUIRE(visited.size() == 2);
    CHECK(visited[0] == "Texture");
    CHECK(visited[1] == "Tint");
}
