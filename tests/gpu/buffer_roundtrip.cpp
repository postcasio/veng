// Buffer round-trip cases (ported from the planset-3 plan 06 one-exe test):
// Buffer::Create -> Upload -> Download returns the same bytes, including an
// offset upload landing at the right place.
//
// Buffer::Download/Upload go through VMA's host-visible mapping
// (vmaCopyMemoryToAllocation / vmaCopyAllocationToMemory) rather than a GPU
// transfer, so no TransferSrc/TransferDst usage is required for them to work —
// but BufferUsage::TransferSrc | TransferDst is set anyway here as the
// realistic usage for a staging-style buffer.

#include <array>
#include <numeric>

#include <doctest/doctest.h>

#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/Types.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "buffer roundtrip: whole-buffer upload/download identity")
{
    constexpr u64 size = 64;

    auto buffer = Buffer::Create(Context, {
        .Name = "Roundtrip Buffer",
        .Size = size,
        .Usage = BufferUsage::TransferSrc | BufferUsage::TransferDst,
    });

    // Whole-buffer upload/download identity: 0, 1, 2, ... 63.
    std::array<u8, size> source{};
    std::iota(source.begin(), source.end(), u8{0});

    buffer->Upload(source);

    const vector<u8> downloaded = buffer->Download();

    REQUIRE(downloaded.size() == size);

    for (u64 i = 0; i < size; i++)
    {
        CAPTURE(i);
        CHECK(downloaded[i] == source[i]);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "buffer roundtrip: offset upload lands at the right place")
{
    constexpr u64 size = 64;

    auto buffer = Buffer::Create(Context, {
        .Name = "Roundtrip Buffer",
        .Size = size,
        .Usage = BufferUsage::TransferSrc | BufferUsage::TransferDst,
    });

    std::array<u8, size> source{};
    std::iota(source.begin(), source.end(), u8{0});

    buffer->Upload(source);

    // Offset upload: overwrite bytes [16, 24) with a distinct pattern and
    // verify it lands at offset 16, leaving the rest untouched.
    constexpr u64 offset = 16;
    const std::array<u8, 8> patch = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8};

    buffer->Upload(patch, offset);

    const vector<u8> afterPatch = buffer->Download();

    REQUIRE(afterPatch.size() == size);

    for (u64 i = 0; i < size; i++)
    {
        const u8 expected = (i >= offset && i < offset + patch.size())
            ? patch[i - offset]
            : source[i];

        CAPTURE(i);
        CHECK(afterPatch[i] == expected);
    }
}
