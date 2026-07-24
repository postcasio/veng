// Mesh-socket cook test: cooks the socket fixture pack through libveng_cook and checks the
// CookedMeshSocket table the mesh importer emits — which authored nodes become sockets and which
// do not (a mesh node, a camera node, the scene root, and a skin's joints), the composition of a
// socket's ancestor chain, the orientation contract (-Z forward, +Y up), and that import.scale
// moves a socket exactly as far as it moves the vertices.

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
    // The socket table of one cooked mesh blob, keyed by name.
    struct DecodedSockets
    {
        CookedMeshHeader Header{};
        vector<CookedMeshSocket> Sockets;

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

    DecodedSockets Decode(const std::span<const u8> blob)
    {
        DecodedSockets decoded;
        std::memcpy(&decoded.Header, blob.data(), sizeof(decoded.Header));

        const usize cursor = sizeof(decoded.Header) +
                             decoded.Header.AttributeCount * sizeof(CookedVertexAttribute) +
                             decoded.Header.SubMeshCount * sizeof(CookedSubMesh);
        decoded.Sockets.resize(decoded.Header.SocketCount);
        for (u32 i = 0; i < decoded.Header.SocketCount; ++i)
        {
            std::memcpy(&decoded.Sockets[i], blob.data() + cursor + i * sizeof(CookedMeshSocket),
                        sizeof(CookedMeshSocket));
        }
        return decoded;
    }

    // The mesh's first vertex position, read out of the interleaved vertex region. The fixture
    // triangle's first vertex is the origin and its second is (1, 0, 0), so the second is what
    // import.scale is visible on.
    vec3 VertexPosition(const std::span<const u8> blob, const CookedMeshHeader& header, u32 index)
    {
        const usize cursor = sizeof(CookedMeshHeader) +
                             header.AttributeCount * sizeof(CookedVertexAttribute) +
                             header.SubMeshCount * sizeof(CookedSubMesh) +
                             header.SocketCount * sizeof(CookedMeshSocket) +
                             static_cast<usize>(index) * header.VertexStride;
        f32 xyz[3] = {};
        std::memcpy(xyz, blob.data() + cursor, sizeof(xyz));
        return vec3(xyz[0], xyz[1], xyz[2]);
    }

    // Rotating v by the socket's quaternion, so a rotation is checked by what it does rather
    // than by its raw components (which have two spellings for one orientation).
    vec3 Rotated(const CookedMeshSocket& socket, const vec3& v)
    {
        const quat q(socket.Rotation[3], socket.Rotation[0], socket.Rotation[1],
                     socket.Rotation[2]);
        return q * v;
    }

    void CheckVec(const vec3& actual, const vec3& expected)
    {
        CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-4));
        CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-4));
        CHECK(actual.z == doctest::Approx(expected.z).epsilon(1e-4));
    }
}

TEST_CASE("Cooker: authored empties become named mesh sockets; geometry and camera nodes do not")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_sockets.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(fixtureDir / "socket_pack.json", outArchive).has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x2D11});
    REQUIRE(entry.has_value());

    const DecodedSockets decoded = Decode(entry->Blob);
    CHECK(decoded.Header.Version == CookedMeshVersion);

    // sockets.gltf authors seven nodes: the scene root, a mesh node, a camera node, and four
    // empties. Only the empties are sockets.
    REQUIRE(decoded.Header.SocketCount == 4);
    CHECK(decoded.Find("Root") == nullptr);
    CHECK(decoded.Find("Body") == nullptr);
    CHECK(decoded.Find("Eye") == nullptr);

    // Sorted by name, so a lookup can binary-search and the blob is stable run to run.
    CHECK(std::strcmp(decoded.Sockets[0].Name, "Mount_A") == 0);
    CHECK(std::strcmp(decoded.Sockets[1].Name, "Mount_B") == 0);
    CHECK(std::strcmp(decoded.Sockets[2].Name, "Mount_C") == 0);
    CHECK(std::strcmp(decoded.Sockets[3].Name, "Rack") == 0);

    // Mount_A is a plain translated empty: position verbatim, no rotation.
    const CookedMeshSocket* mountA = decoded.Find("Mount_A");
    REQUIRE(mountA != nullptr);
    CheckVec(vec3(mountA->Position[0], mountA->Position[1], mountA->Position[2]),
             vec3(1.0f, 2.0f, 3.0f));
    CheckVec(Rotated(*mountA, vec3(0.0f, 0.0f, -1.0f)), vec3(0.0f, 0.0f, -1.0f));
    CheckVec(vec3(mountA->Scale[0], mountA->Scale[1], mountA->Scale[2]), vec3(1.0f));

    // Mount_B is rotated 90 degrees about +Y. The orientation contract is that local -Z is the
    // socket's forward and local +Y its up, so a quarter turn about Y aims forward down -X and
    // leaves up alone.
    const CookedMeshSocket* mountB = decoded.Find("Mount_B");
    REQUIRE(mountB != nullptr);
    CheckVec(Rotated(*mountB, vec3(0.0f, 0.0f, -1.0f)), vec3(-1.0f, 0.0f, 0.0f));
    CheckVec(Rotated(*mountB, vec3(0.0f, 1.0f, 0.0f)), vec3(0.0f, 1.0f, 0.0f));

    // Mount_C sits under Rack, which is itself translated and rotated. The socket carries the
    // composed ancestor chain, not its own node transform: Rack's +Y 90 degree turn maps the
    // child's local +X offset onto -Z.
    const CookedMeshSocket* mountC = decoded.Find("Mount_C");
    REQUIRE(mountC != nullptr);
    CheckVec(vec3(mountC->Position[0], mountC->Position[1], mountC->Position[2]),
             vec3(0.0f, 5.0f, -2.0f));
    CheckVec(Rotated(*mountC, vec3(0.0f, 0.0f, -1.0f)), vec3(-1.0f, 0.0f, 0.0f));

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: import.scale moves a socket exactly as far as it moves the vertices")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_sockets_scaled.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(fixtureDir / "socket_pack.json", outArchive).has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> plain = reader->Find(AssetId{0x2D11});
    const optional<ArchiveEntry> scaled = reader->Find(AssetId{0x2D12});
    REQUIRE(plain.has_value());
    REQUIRE(scaled.has_value());

    const DecodedSockets plainSockets = Decode(plain->Blob);
    const DecodedSockets scaledSockets = Decode(scaled->Blob);

    // The vertex the fixture puts at (1, 0, 0) doubles under import.scale 2.
    CheckVec(VertexPosition(plain->Blob, plainSockets.Header, 1), vec3(1.0f, 0.0f, 0.0f));
    CheckVec(VertexPosition(scaled->Blob, scaledSockets.Header, 1), vec3(2.0f, 0.0f, 0.0f));

    // So does the socket under the transformed parent — the same space as the vertices.
    const CookedMeshSocket* mountC = scaledSockets.Find("Mount_C");
    REQUIRE(mountC != nullptr);
    CheckVec(vec3(mountC->Position[0], mountC->Position[1], mountC->Position[2]),
             vec3(0.0f, 10.0f, -4.0f));

    // Scale is a distance, not an orientation: the rotation is untouched.
    CheckVec(Rotated(*mountC, vec3(0.0f, 0.0f, -1.0f)), vec3(-1.0f, 0.0f, 0.0f));

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: a skin's joints produce no sockets")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_skinned_sockets.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(fixtureDir / "socket_pack.json", outArchive).has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x2D13});
    REQUIRE(entry.has_value());

    const DecodedSockets decoded = Decode(entry->Blob);
    CHECK(decoded.Header.SkeletonId == 0x2D01);

    // A joint is exactly a node with no mesh and no camera, so without the joint exclusion an
    // entire rig would become sockets. The fixture's two joints and its export root produce
    // none; the one genuine empty beside them still does.
    REQUIRE(decoded.Header.SocketCount == 1);
    CHECK(std::strcmp(decoded.Sockets[0].Name, "Mount_Head") == 0);
    CHECK(decoded.Find("Joint_Root") == nullptr);
    CHECK(decoded.Find("Joint_Tip") == nullptr);
    CHECK(decoded.Find("Root") == nullptr);

    std::filesystem::remove(outArchive);
}
