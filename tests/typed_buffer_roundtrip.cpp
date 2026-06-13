// Typed-buffer round-trip test (planset-3, plan 06): VertexBuffer<V> and
// StorageBuffer<T> Upload -> Download identity.
//
// This is the primary device-side coverage of the typed-buffer size arithmetic
// (count * sizeof(T)) that plan 02 deliberately left untested device-free: a
// wrong size computation either produces a too-small/too-large allocation or a
// download whose byte layout doesn't match what was uploaded, and this test
// would mismatch.
//
// Buffer::Download/Upload go through VMA's host-visible mapping, not a GPU
// transfer, so the TransferDst-only usage VertexBuffer/StorageBuffer fix
// internally is sufficient for GetBuffer()->Download() to work — no
// TransferSrc needed.
//
// Skips cleanly (exit 77, ctest reports it as skipped) on a machine with no
// usable Vulkan ICD, via Test::HasVulkanDriver() (planset-3, plan 01/06).

#include <cstdio>
#include <cstring>

#include <Veng/Renderer/TypedBuffers.h>
#include <Veng/Renderer/Types.h>

#include <support/GpuContext.h>
#include <support/GpuProbe.h>

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

int main()
{
    if (!Test::HasVulkanDriver())
        return 77;

    Test::GpuContext gpu("Typed Buffer Roundtrip Test");
    Context& context = gpu.Get();
    (void)context;

    int status = 0;

    // --- VertexBuffer<Vertex> ---
    {
        constexpr usize count = 3;

        const vector<Vertex> source = {
            {.Position = {0.0f, 0.5f, 0.0f}, .Color = {1.0f, 0.0f, 0.0f, 1.0f}},
            {.Position = {-0.5f, -0.5f, 0.0f}, .Color = {0.0f, 1.0f, 0.0f, 1.0f}},
            {.Position = {0.5f, -0.5f, 0.0f}, .Color = {0.0f, 0.0f, 1.0f, 1.0f}},
        };

        auto vertexBuffer = VertexBuffer<Vertex>::Create("Vertices", count);
        vertexBuffer.Upload(source);

        const vector<u8> downloaded = vertexBuffer.GetBuffer()->Download();

        if (downloaded.size() != count * sizeof(Vertex))
        {
            std::fprintf(stderr, "FAIL: VertexBuffer download size %zu, expected %zu\n",
                          downloaded.size(), count * sizeof(Vertex));
            status = 1;
        }
        else if (std::memcmp(downloaded.data(), source.data(), downloaded.size()) != 0)
        {
            std::fprintf(stderr, "FAIL: VertexBuffer download bytes do not match upload\n");
            status = 1;
        }
    }

    // --- StorageBuffer<Particle>, including an offset (firstElement) upload ---
    if (status == 0)
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

        auto storageBuffer = StorageBuffer<Particle>::Create("Particles", count);

        // Upload the first two elements, then the last two at firstElement=2 —
        // exercises the firstElement * sizeof(T) offset arithmetic.
        storageBuffer.Upload(std::span(source).subspan(0, 2), 0);
        storageBuffer.Upload(std::span(source).subspan(2, 2), 2);

        const vector<u8> downloaded = storageBuffer.GetBuffer()->Download();

        if (downloaded.size() != count * sizeof(Particle))
        {
            std::fprintf(stderr, "FAIL: StorageBuffer download size %zu, expected %zu\n",
                          downloaded.size(), count * sizeof(Particle));
            status = 1;
        }
        else if (std::memcmp(downloaded.data(), source.data(), downloaded.size()) != 0)
        {
            std::fprintf(stderr, "FAIL: StorageBuffer download bytes do not match upload\n");
            status = 1;
        }
    }

    if (status == 0)
    {
        std::printf("OK: typed buffer (VertexBuffer/StorageBuffer) upload/download roundtrip verified\n");
    }

    return status;
}
