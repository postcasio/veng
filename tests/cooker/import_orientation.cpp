// Import-orientation cook test: cooks one model twice — once with no declared convention and once
// declaring the source's own forward/up axes — and checks that every value the importer derives
// from that file crosses the same rotation: vertex positions, normals and tangent directions, the
// tangent's handedness sign, the triangle winding, the socket table's positions and orientations,
// and a collision shape's welded geometry. A partial rotation is worse than none — a socket that
// stays put while its geometry turns places whatever attaches there in mid-air — so the cases here
// compare the two cooks value by value rather than spot-checking one output.

#include <cmath>
#include <cstring>
#include <filesystem>
#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // The rotation the fixtures' "forward": "+Z" declaration resolves to: a half turn about +Y,
    // which is the reconciliation a source authored nose-at-+Z needs against the engine's -Z.
    vec3 HalfTurnY(const vec3& v)
    {
        return vec3(-v.x, v.y, -v.z);
    }

    // The rotation "forward": "+X" resolves to: a quarter turn about +Y, taking +X onto -Z. The
    // quarter turn is the case that would lose precision through a quaternion, so the collision
    // fixture uses it.
    vec3 QuarterTurnY(const vec3& v)
    {
        return vec3(v.z, v.y, -v.x);
    }

    // One decoded mesh blob: the header, the socket table, and the interleaved vertex/index regions.
    struct DecodedMesh
    {
        CookedMeshHeader Header{};
        vector<CookedMeshSocket> Sockets;
        vector<u8> Vertices;
        vector<u32> Indices;

        [[nodiscard]] vec3 Attribute(u32 vertex, usize byteOffset) const
        {
            f32 xyz[3] = {};
            std::memcpy(xyz, Vertices.data() + vertex * Header.VertexStride + byteOffset,
                        sizeof(xyz));
            return vec3(xyz[0], xyz[1], xyz[2]);
        }

        [[nodiscard]] vec3 Position(u32 vertex) const { return Attribute(vertex, 0); }
        [[nodiscard]] vec3 Normal(u32 vertex) const { return Attribute(vertex, 12); }
        [[nodiscard]] vec3 Tangent(u32 vertex) const { return Attribute(vertex, 24); }

        [[nodiscard]] f32 Handedness(u32 vertex) const
        {
            f32 w = 0.0f;
            std::memcpy(&w, Vertices.data() + vertex * Header.VertexStride + 36, sizeof(w));
            return w;
        }

        [[nodiscard]] const CookedMeshSocket* Find(const char* name) const
        {
            for (const CookedMeshSocket& socket : Sockets)
            {
                if (std::strcmp(socket.Name, name) == 0)
                {
                    return &socket;
                }
            }
            return nullptr;
        }
    };

    DecodedMesh DecodeMesh(const std::span<const u8> blob)
    {
        DecodedMesh decoded;
        std::memcpy(&decoded.Header, blob.data(), sizeof(decoded.Header));

        usize cursor = sizeof(decoded.Header) +
                       decoded.Header.AttributeCount * sizeof(CookedVertexAttribute) +
                       decoded.Header.SubMeshCount * sizeof(CookedSubMesh);
        decoded.Sockets.resize(decoded.Header.SocketCount);
        for (u32 i = 0; i < decoded.Header.SocketCount; ++i)
        {
            std::memcpy(&decoded.Sockets[i], blob.data() + cursor, sizeof(CookedMeshSocket));
            cursor += sizeof(CookedMeshSocket);
        }

        const usize vertexBytes =
            static_cast<usize>(decoded.Header.VertexCount) * decoded.Header.VertexStride;
        decoded.Vertices.resize(vertexBytes);
        std::memcpy(decoded.Vertices.data(), blob.data() + cursor, vertexBytes);
        cursor += vertexBytes;

        decoded.Indices.resize(decoded.Header.IndexCount);
        if (decoded.Header.IndexCount > 0)
        {
            std::memcpy(decoded.Indices.data(), blob.data() + cursor,
                        decoded.Indices.size() * sizeof(u32));
        }
        return decoded;
    }

    // One decoded collision blob.
    struct DecodedShape
    {
        CookedCollisionShapeHeader Header{};
        vector<vec3> Points;
        vector<u32> Indices;
    };

    DecodedShape DecodeShape(const std::span<const u8> blob)
    {
        DecodedShape decoded;
        std::memcpy(&decoded.Header, blob.data(), sizeof(decoded.Header));

        usize cursor = sizeof(decoded.Header);
        decoded.Points.resize(decoded.Header.PointCount);
        for (u32 i = 0; i < decoded.Header.PointCount; ++i)
        {
            f32 xyz[3] = {};
            std::memcpy(xyz, blob.data() + cursor, sizeof(xyz));
            cursor += sizeof(xyz);
            decoded.Points[i] = vec3(xyz[0], xyz[1], xyz[2]);
        }
        decoded.Indices.resize(decoded.Header.IndexCount);
        if (decoded.Header.IndexCount > 0)
        {
            std::memcpy(decoded.Indices.data(), blob.data() + cursor,
                        decoded.Indices.size() * sizeof(u32));
        }
        return decoded;
    }

    void CheckVec(const vec3& actual, const vec3& expected)
    {
        CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-5));
        CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-5));
        CHECK(actual.z == doctest::Approx(expected.z).epsilon(1e-5));
    }

    // Rotating a probe direction by a socket's orientation, so an orientation is checked by what it
    // does rather than by its raw components (which have two spellings for one rotation).
    vec3 Rotated(const CookedMeshSocket& socket, const vec3& v)
    {
        const quat q(socket.Rotation[3], socket.Rotation[0], socket.Rotation[1],
                     socket.Rotation[2]);
        return q * v;
    }

    // Cooks the orientation fixture pack once per test case, into that case's own archive.
    Result<ArchiveReader> CookOrientationPack(const char* archiveName)
    {
        const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / archiveName;

        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        REQUIRE(cooker.CookPack(fixtureDir / "orientation_pack.json", outArchive).has_value());
        return ArchiveReader::Open(outArchive);
    }
}

TEST_CASE("Cooker: a declared orientation rotates positions, normals and tangent directions")
{
    const Result<ArchiveReader> reader =
        CookOrientationPack("veng_cooker_orientation_mesh.vengpack");
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> plain = reader->Find(AssetId{0x2E01});
    const optional<ArchiveEntry> turned = reader->Find(AssetId{0x2E02});
    REQUIRE(plain.has_value());
    REQUIRE(turned.has_value());

    const DecodedMesh a = DecodeMesh(plain->Blob);
    const DecodedMesh b = DecodeMesh(turned->Blob);

    // The two cooks read the same model with the same post-process flags, so they differ only by
    // the rotation: the vertex and index counts, the stride, and the submesh table are untouched.
    REQUIRE(a.Header.VertexCount == b.Header.VertexCount);
    CHECK(a.Header.VertexStride == b.Header.VertexStride);
    CHECK(a.Header.IndexCount == b.Header.IndexCount);
    CHECK(a.Header.SubMeshCount == b.Header.SubMeshCount);

    for (u32 v = 0; v < a.Header.VertexCount; ++v)
    {
        CheckVec(b.Position(v), HalfTurnY(a.Position(v)));
        CheckVec(b.Normal(v), HalfTurnY(a.Normal(v)));
        CheckVec(b.Tangent(v), HalfTurnY(a.Tangent(v)));

        // The handedness sign is the bitangent's chirality. A declared orientation is a proper
        // rotation, so it cannot flip it — a rotated w would mirror every normal-mapped surface.
        CHECK(b.Handedness(v) == a.Handedness(v));
    }

    // A proper rotation preserves triangle winding, so the index buffer is untouched: a mirrored
    // reconciliation would need every triangle reversed and would back-face-cull the whole hull.
    CHECK(a.Indices == b.Indices);
}

TEST_CASE("Cooker: a declared orientation moves a socket exactly as it moves the geometry")
{
    const Result<ArchiveReader> reader =
        CookOrientationPack("veng_cooker_orientation_sockets.vengpack");
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> plain = reader->Find(AssetId{0x2E03});
    const optional<ArchiveEntry> turned = reader->Find(AssetId{0x2E04});
    REQUIRE(plain.has_value());
    REQUIRE(turned.has_value());

    const DecodedMesh a = DecodeMesh(plain->Blob);
    const DecodedMesh b = DecodeMesh(turned->Blob);

    REQUIRE(a.Header.SocketCount == 4);
    REQUIRE(b.Header.SocketCount == a.Header.SocketCount);

    // Sorted by name in both, so the tables correspond entry for entry.
    for (u32 i = 0; i < a.Header.SocketCount; ++i)
    {
        CHECK(std::strcmp(a.Sockets[i].Name, b.Sockets[i].Name) == 0);

        const vec3 place(a.Sockets[i].Position[0], a.Sockets[i].Position[1],
                         a.Sockets[i].Position[2]);
        CheckVec(vec3(b.Sockets[i].Position[0], b.Sockets[i].Position[1], b.Sockets[i].Position[2]),
                 HalfTurnY(place));

        // The socket's own forward and up cross the same rotation as its position, so a socket
        // keeps pointing at the feature of the geometry it was authored against.
        CheckVec(Rotated(b.Sockets[i], vec3(0.0f, 0.0f, -1.0f)),
                 HalfTurnY(Rotated(a.Sockets[i], vec3(0.0f, 0.0f, -1.0f))));
        CheckVec(Rotated(b.Sockets[i], vec3(0.0f, 1.0f, 0.0f)),
                 HalfTurnY(Rotated(a.Sockets[i], vec3(0.0f, 1.0f, 0.0f))));

        // A rotation is not a scale: the socket's own scale is untouched.
        CheckVec(vec3(b.Sockets[i].Scale[0], b.Sockets[i].Scale[1], b.Sockets[i].Scale[2]),
                 vec3(a.Sockets[i].Scale[0], a.Sockets[i].Scale[1], a.Sockets[i].Scale[2]));
    }

    // And the vertices moved with them: the fixture's second vertex sits at (1, 0, 0).
    CheckVec(a.Position(1), vec3(1.0f, 0.0f, 0.0f));
    CheckVec(b.Position(1), vec3(-1.0f, 0.0f, 0.0f));

    // The declared half turn is what a source authored nose-at-+Z needs: a socket whose forward
    // aimed down the source's +Z now aims down the engine's -Z.
    const CookedMeshSocket* mountA = b.Find("Mount_A");
    REQUIRE(mountA != nullptr);
    CheckVec(Rotated(*mountA, vec3(0.0f, 0.0f, -1.0f)), vec3(0.0f, 0.0f, 1.0f));
}

TEST_CASE("Cooker: a declared orientation rotates a collision shape's welded geometry")
{
    const Result<ArchiveReader> reader =
        CookOrientationPack("veng_cooker_orientation_collision.vengpack");
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> plain = reader->Find(AssetId{0x2E05});
    const optional<ArchiveEntry> turned = reader->Find(AssetId{0x2E06});
    REQUIRE(plain.has_value());
    REQUIRE(turned.has_value());

    const DecodedShape a = DecodeShape(plain->Blob);
    const DecodedShape b = DecodeShape(turned->Blob);

    // Welding is by position and a rotation is injective, so the same source vertices weld to the
    // same classes in the same order — the point tables correspond entry for entry.
    CHECK(a.Header.Mode == b.Header.Mode);
    REQUIRE(a.Header.PointCount == b.Header.PointCount);
    for (usize i = 0; i < a.Points.size(); ++i)
    {
        CheckVec(b.Points[i], QuarterTurnY(a.Points[i]));
    }

    // A quarter turn about +Y is a signed permutation, so the cooked values are exact swaps and
    // sign flips of the source's rather than a resampling: the cube's corners stay on ±0.5.
    for (const vec3 point : b.Points)
    {
        CHECK(std::abs(point.x) == 0.5f);
        CHECK(std::abs(point.y) == 0.5f);
        CHECK(std::abs(point.z) == 0.5f);
    }

    CHECK(a.Indices == b.Indices);
}

TEST_CASE("Cooker: declaring the engine's own convention cooks byte-identically to declaring none")
{
    const Result<ArchiveReader> reader =
        CookOrientationPack("veng_cooker_orientation_default.vengpack");
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> absent = reader->Find(AssetId{0x2E01});
    const optional<ArchiveEntry> restated = reader->Find(AssetId{0x2E07});
    REQUIRE(absent.has_value());
    REQUIRE(restated.has_value());

    // The default is a no-op down to the bytes, which is what lets every existing asset keep
    // cooking to the archive it already cooked to.
    const vector<u8> absentBytes(absent->Blob.begin(), absent->Blob.end());
    const vector<u8> restatedBytes(restated->Blob.begin(), restated->Blob.end());
    CHECK(absentBytes == restatedBytes);
}

TEST_CASE("Cooker: a declared orientation on a skinned mesh is a located cook error")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_orientation_skinned.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const VoidResult cooked =
        cooker.CookPack(fixtureDir / "orientation_skinned_pack.json", outArchive);

    // The bind pose and the animation channels are cooked from the same model by their own
    // importers and keep the source's convention, so rotating the geometry alone would desync the
    // skin. Refused at cook time rather than half-applied at runtime.
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("not supported on a skinned mesh") != string::npos);
}

TEST_CASE("Cooker: an unknown axis name is a located cook error")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_orientation_axis.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const VoidResult cooked =
        cooker.CookPack(fixtureDir / "orientation_bad_axis_pack.json", outArchive);

    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("'import.orientation.forward' is not an axis name") != string::npos);
}

TEST_CASE("Cooker: a forward and an up on one axis is a located cook error")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_orientation_parallel.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const VoidResult cooked =
        cooker.CookPack(fixtureDir / "orientation_parallel_pack.json", outArchive);

    // One axis does not determine a rotation: +Y forward with -Y up leaves the remaining two axes
    // free, so the declaration is rejected rather than resolved arbitrarily.
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("must be perpendicular") != string::npos);
}
