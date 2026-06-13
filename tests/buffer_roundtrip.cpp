// Buffer round-trip test (planset-3, plan 06): Buffer::Create -> Upload ->
// Download returns the same bytes, including an offset upload landing at the
// right place.
//
// Buffer::Download/Upload go through VMA's host-visible mapping
// (vmaCopyMemoryToAllocation / vmaCopyAllocationToMemory) rather than a GPU
// transfer, so no TransferSrc/TransferDst usage is required for them to work —
// but BufferUsage::TransferSrc | TransferDst is set anyway here as the
// realistic usage for a staging-style buffer.
//
// Skips cleanly (exit 77, ctest reports it as skipped) on a machine with no
// usable Vulkan ICD, via Test::HasVulkanDriver() (planset-3, plan 01/06).

#include <array>
#include <cstdio>
#include <numeric>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Types.h>

#include <support/GpuContext.h>
#include <support/GpuProbe.h>

using namespace Veng;
using namespace Veng::Renderer;

int main()
{
    if (!Test::HasVulkanDriver())
        return 77;

    Test::GpuContext gpu("Buffer Roundtrip Test");
    Context& context = gpu.Get();

    int status = 0;
    {
        constexpr u64 size = 64;

        auto buffer = Buffer::Create(context, {
            .Name = "Roundtrip Buffer",
            .Size = size,
            .Usage = BufferUsage::TransferSrc | BufferUsage::TransferDst,
        });

        // Whole-buffer upload/download identity: 0, 1, 2, ... 63.
        std::array<u8, size> source{};
        std::iota(source.begin(), source.end(), u8{0});

        buffer->Upload(source);

        const vector<u8> downloaded = buffer->Download();

        if (downloaded.size() != size)
        {
            std::fprintf(stderr, "FAIL: unexpected download size %zu (expected %llu)\n",
                          downloaded.size(), static_cast<unsigned long long>(size));
            status = 1;
        }
        else
        {
            for (u64 i = 0; i < size && status == 0; i++)
            {
                if (downloaded[i] != source[i])
                {
                    std::fprintf(stderr, "FAIL: byte %llu = %u, expected %u\n",
                                  static_cast<unsigned long long>(i), downloaded[i], source[i]);
                    status = 1;
                }
            }
        }

        // Offset upload: overwrite bytes [16, 24) with a distinct pattern and
        // verify it lands at offset 16, leaving the rest untouched.
        if (status == 0)
        {
            constexpr u64 offset = 16;
            const std::array<u8, 8> patch = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8};

            buffer->Upload(patch, offset);

            const vector<u8> afterPatch = buffer->Download();

            for (u64 i = 0; i < size && status == 0; i++)
            {
                const u8 expected = (i >= offset && i < offset + patch.size())
                    ? patch[i - offset]
                    : source[i];

                if (afterPatch[i] != expected)
                {
                    std::fprintf(stderr, "FAIL: byte %llu (after patch) = %u, expected %u\n",
                                  static_cast<unsigned long long>(i), afterPatch[i], expected);
                    status = 1;
                }
            }
        }
    }

    if (status == 0)
    {
        std::printf("OK: buffer upload/download roundtrip verified (incl. offset upload)\n");
    }

    return status;
}
