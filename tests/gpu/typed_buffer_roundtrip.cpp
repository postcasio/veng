// Typed-buffer round-trip cases: VertexBuffer<V> and StorageBuffer<T>
// Upload -> Download identity.
//
// Device-side coverage of the typed-buffer size arithmetic (count * sizeof(T)):
// a wrong size computation either produces a too-small/too-large allocation or
// a download whose byte layout doesn't match what was uploaded, and these cases
// would mismatch.
//
// Buffer::Download/Upload go through VMA's host-visible mapping, not a GPU
// transfer, so the TransferDst-only usage VertexBuffer/StorageBuffer fix
// internally is sufficient for GetBuffer()->Download() to work — no
// TransferSrc needed.

#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Renderer/TypedBuffers.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Simple trivially-copyable vertex type: position + colour.
    struct Vertex
    {
        vec3 Position;
        vec4 Color;
    };

    // Simple trivially-copyable storage element.
    struct Particle
    {
        vec2 Position;
        vec2 Velocity;
        f32 Lifetime;
        f32 Padding[3]; // keep std430-friendly 16-byte alignment, matches house convention
    };
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "typed buffer roundtrip: VertexBuffer<Vertex>")
{
    constexpr usize count = 3;

    const vector<Vertex> source = {
        {.Position = {0.0f, 0.5f, 0.0f}, .Color = {1.0f, 0.0f, 0.0f, 1.0f}},
        {.Position = {-0.5f, -0.5f, 0.0f}, .Color = {0.0f, 1.0f, 0.0f, 1.0f}},
        {.Position = {0.5f, -0.5f, 0.0f}, .Color = {0.0f, 0.0f, 1.0f, 1.0f}},
    };

    auto vertexBuffer = VertexBuffer<Vertex>::Create(Context, "Vertices", count);
    vertexBuffer.UploadSync(source);

    const vector<u8> downloaded = vertexBuffer.GetBuffer()->Download();

    REQUIRE(downloaded.size() == count * sizeof(Vertex));
    CHECK(std::memcmp(downloaded.data(), source.data(), downloaded.size()) == 0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "typed buffer roundtrip: StorageBuffer<Particle> with firstElement offset")
{
    constexpr usize count = 4;

    vector<Particle> source(count);
    for (usize i = 0; i < count; i++)
    {
        source[i] = Particle{
            .Position = {static_cast<f32>(i), static_cast<f32>(i) * 2.0f},
            .Velocity = {0.1f * static_cast<f32>(i), -0.1f * static_cast<f32>(i)},
            .Lifetime = static_cast<f32>(i) + 1.0f,
            .Padding = {0, 0, 0},
        };
    }

    auto storageBuffer = StorageBuffer<Particle>::Create(Context, "Particles", count);

    // Upload the first two elements, then the last two at firstElement=2 —
    // exercises the firstElement * sizeof(T) offset arithmetic.
    storageBuffer.UploadSync(std::span(source).subspan(0, 2), 0);
    storageBuffer.UploadSync(std::span(source).subspan(2, 2), 2);

    const vector<u8> downloaded = storageBuffer.GetBuffer()->Download();

    REQUIRE(downloaded.size() == count * sizeof(Particle));
    CHECK(std::memcmp(downloaded.data(), source.data(), downloaded.size()) == 0);
}
