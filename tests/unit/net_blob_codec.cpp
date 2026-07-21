// Typed-blob codec cases. BlobCodec is pure value logic over Net::Blob: EncodeBlob/DecodeBlob
// memcpy a trivially-copyable payload under an explicit tag, EncodeBlobRecord/DecodeBlobRecord run
// a reflected value through the field walkers. These pin the two round-trips and the decode
// contracts a wire payload depends on — the tag as a discriminator distinct from the layout type,
// the `>=` size test, and nullopt (never an assert) for every rejection. No socket, no host.

#include <doctest/doctest.h>

#include <Veng/Net/BlobCodec.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // A fixed-layout wire payload: trivially copyable, deliberately *not* reflected, which is the
    // common shape and the reason the tag cannot be derived from the payload type.
    struct ArrivalPayload
    {
        f32 X = 0.0f;
        f32 Y = 0.0f;
        u32 Flags = 0;

        bool operator==(const ArrivalPayload&) const = default;
    };

    // A second fixed-layout payload of a different size, so a cross-tag case can also exercise the
    // length arm independently.
    struct NamePayload
    {
        char Text[16] = {};
        u32 Length = 0;
    };

    // A reflected record for the walker variants.
    struct RouteRequest
    {
        u32 Hops = 0;
        f32 Cost = 0.0f;
    };

    // The two drift shapes model one logical record as two builds see it: same authored TypeId
    // (a reflected type keeps its id while its fields evolve), one field added. They are never
    // registered in the same TypeRegistry — that would be a genuine id collision — so each case
    // builds the registry for the build it is standing in.
    struct DriftOld
    {
        u32 A = 0;
    };

    struct DriftNew
    {
        u32 A = 0;
        f32 B = 0.0f;
    };
}

VE_REFLECT(::RouteRequest, 0x2E5A6C1B47D0F933ULL)
VE_FIELD(Hops, .DisplayName = "Hops")
VE_FIELD(Cost, .DisplayName = "Cost")
VE_REFLECT_END();

VE_REFLECT(::DriftOld, 0x8B3F0C27A61D4E55ULL)
VE_FIELD(A, .DisplayName = "A")
VE_REFLECT_END();

VE_REFLECT(::DriftNew, 0x8B3F0C27A61D4E55ULL)
VE_FIELD(A, .DisplayName = "A")
VE_FIELD(B, .DisplayName = "B")
VE_REFLECT_END();

namespace
{
    // The discriminator the fixed-layout cases ride: a reflected type's id that describes none of
    // their layouts, which is the shape a cross-tagging consumer picks.
    constexpr TypeId ArrivalTag = TypeIdOf<RouteRequest>();
}

TEST_CASE("blob codec: a fixed-layout payload round-trips under its tag")
{
    const ArrivalPayload src{.X = 3.5f, .Y = -2.25f, .Flags = 7};
    const Blob blob = EncodeBlob(src, ArrivalTag);

    CHECK(blob.Type == ArrivalTag);
    CHECK(blob.Bytes.size() == sizeof(ArrivalPayload));

    const optional<ArrivalPayload> decoded = DecodeBlob<ArrivalPayload>(blob, ArrivalTag);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == src);
}

TEST_CASE("blob codec: a wrong tag decodes nullopt")
{
    const Blob blob = EncodeBlob(ArrivalPayload{.X = 1.0f}, ArrivalTag);

    CHECK_FALSE(DecodeBlob<ArrivalPayload>(blob, TypeIdOf<DriftOld>()).has_value());
    CHECK_FALSE(DecodeBlob<ArrivalPayload>(blob, InvalidTypeId).has_value());
}

TEST_CASE("blob codec: a short payload decodes nullopt, never a partial read")
{
    Blob blob = EncodeBlob(ArrivalPayload{.X = 1.0f, .Y = 2.0f, .Flags = 3}, ArrivalTag);
    blob.Bytes.pop_back();

    CHECK_FALSE(DecodeBlob<ArrivalPayload>(blob, ArrivalTag).has_value());
}

TEST_CASE("blob codec: an empty blob decodes nullopt in both forms")
{
    TypeRegistry registry;
    registry.Register<RouteRequest>();

    const Blob empty;
    CHECK(empty.Type == InvalidTypeId);
    CHECK_FALSE(DecodeBlob<ArrivalPayload>(empty, ArrivalTag).has_value());
    CHECK_FALSE(DecodeBlob<ArrivalPayload>(empty, InvalidTypeId).has_value());
    CHECK_FALSE(DecodeBlobRecord<RouteRequest>(empty, registry).has_value());

    // An empty blob carrying the record form's own tag is still nothing to decode.
    const Blob taggedEmpty{.Type = TypeIdOf<RouteRequest>()};
    CHECK_FALSE(DecodeBlobRecord<RouteRequest>(taggedEmpty, registry).has_value());
}

TEST_CASE("blob codec: a payload tagged with a type other than its layout type")
{
    // The cross-tagging contract: a consumer discriminates two payloads on a shared channel by
    // tagging one with a type it does not carry. The blob round-trips under the discriminator and
    // is rejected under the payload's own layout type, which is what keeps the two apart.
    const NamePayload src{.Text = "peer", .Length = 4};
    const Blob blob = EncodeBlob(src, ArrivalTag);

    const optional<NamePayload> underTag = DecodeBlob<NamePayload>(blob, ArrivalTag);
    REQUIRE(underTag.has_value());
    CHECK(underTag->Length == 4u);
    CHECK(string(underTag->Text) == "peer");

    CHECK_FALSE(DecodeBlob<NamePayload>(blob, TypeIdOf<DriftOld>()).has_value());
}

TEST_CASE("blob codec: an oversized payload under the expected tag decodes its prefix")
{
    // The size test is `>=`, not `==`: a longer payload sharing the tag decodes its leading
    // sizeof(T) bytes. A shared channel where a shorter payload reads as absent depends on this
    // asymmetry, so the loose end is pinned as contract rather than tolerated as an accident.
    const ArrivalPayload src{.X = 9.0f, .Y = 8.0f, .Flags = 42};
    Blob blob = EncodeBlob(src, ArrivalTag);
    blob.Bytes.insert(blob.Bytes.end(), {0xAA, 0xBB, 0xCC});

    const optional<ArrivalPayload> decoded = DecodeBlob<ArrivalPayload>(blob, ArrivalTag);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == src);
}

TEST_CASE("blob codec: a reflected record round-trips through the walkers")
{
    TypeRegistry registry;
    registry.Register<RouteRequest>();

    const RouteRequest src{.Hops = 3, .Cost = 12.5f};
    const Blob blob = EncodeBlobRecord(src, registry);

    CHECK(blob.Type == TypeIdOf<RouteRequest>());
    CHECK_FALSE(blob.IsEmpty());

    const optional<RouteRequest> decoded = DecodeBlobRecord<RouteRequest>(blob, registry);
    REQUIRE(decoded.has_value());
    CHECK(decoded->Hops == 3u);
    CHECK(decoded->Cost == doctest::Approx(12.5f));
}

TEST_CASE("blob codec: a record blob under a foreign tag decodes nullopt")
{
    TypeRegistry registry;
    registry.Register<RouteRequest>();

    Blob blob = EncodeBlobRecord(RouteRequest{.Hops = 1, .Cost = 1.0f}, registry);
    blob.Type = TypeIdOf<DriftOld>();

    CHECK_FALSE(DecodeBlobRecord<RouteRequest>(blob, registry).has_value());
}

TEST_CASE("blob codec: a truncated record decodes nullopt")
{
    TypeRegistry registry;
    registry.Register<RouteRequest>();

    // Three bytes is short of even the leading record count, so the walker runs out of input
    // regardless of how the fields happen to pack.
    Blob blob = EncodeBlobRecord(RouteRequest{.Hops = 5, .Cost = 2.0f}, registry);
    REQUIRE(blob.Bytes.size() > 3);
    blob.Bytes.resize(3);

    CHECK_FALSE(DecodeBlobRecord<RouteRequest>(blob, registry).has_value());
}

TEST_CASE("blob codec: the record form tolerates schema drift in both directions")
{
    // Forward: a newer build's record read by an older one. The field the old shape does not know
    // is skipped, the shared field survives.
    {
        TypeRegistry newer;
        newer.Register<DriftNew>();
        const Blob blob = EncodeBlobRecord(DriftNew{.A = 11, .B = 4.5f}, newer);

        TypeRegistry older;
        older.Register<DriftOld>();
        const optional<DriftOld> decoded = DecodeBlobRecord<DriftOld>(blob, older);
        REQUIRE(decoded.has_value());
        CHECK(decoded->A == 11u);
    }

    // Backward: an older build's record read by a newer one. The field the record omits keeps its
    // default rather than reading garbage.
    {
        TypeRegistry older;
        older.Register<DriftOld>();
        const Blob blob = EncodeBlobRecord(DriftOld{.A = 23}, older);

        TypeRegistry newer;
        newer.Register<DriftNew>();
        const optional<DriftNew> decoded = DecodeBlobRecord<DriftNew>(blob, newer);
        REQUIRE(decoded.has_value());
        CHECK(decoded->A == 23u);
        CHECK(decoded->B == doctest::Approx(0.0f));
    }
}
