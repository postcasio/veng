// The engine's indirect records (Renderer/Types.h) are the bytes a device reads, so a record
// written through the engine type must land where the layout contract says: each field a
// tightly packed 4-byte word, in declaration order.

#include <doctest/doctest.h>

#include <Veng/Renderer/Types.h>

#include <array>
#include <cstring>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The 4-byte word at index @p word of a record's bytes.
    template <typename Record>
    u32 Word(const Record& record, const usize word)
    {
        std::array<std::byte, sizeof(Record)> bytes{};
        std::memcpy(bytes.data(), &record, sizeof(Record));
        u32 value = 0;
        std::memcpy(&value, bytes.data() + (word * sizeof(u32)), sizeof(u32));
        return value;
    }
}

TEST_CASE("an indexed indirect record reads back field by field from its bytes")
{
    const DrawIndexedIndirectCommand record{
        .IndexCount = 36,
        .InstanceCount = 7,
        .FirstIndex = 120,
        .VertexOffset = -5,
        .FirstInstance = 3,
    };
    CHECK(Word(record, 0) == 36);
    CHECK(Word(record, 1) == 7);
    CHECK(Word(record, 2) == 120);
    CHECK(static_cast<i32>(Word(record, 3)) == -5);
    CHECK(Word(record, 4) == 3);
}

TEST_CASE("a non-indexed indirect record reads back field by field from its bytes")
{
    const DrawIndirectCommand record{
        .VertexCount = 6,
        .InstanceCount = 4096,
        .FirstVertex = 12,
        .FirstInstance = 9,
    };
    CHECK(Word(record, 0) == 6);
    CHECK(Word(record, 1) == 4096);
    CHECK(Word(record, 2) == 12);
    CHECK(Word(record, 3) == 9);
}

TEST_CASE("a dispatch indirect record reads back field by field from its bytes")
{
    const DispatchIndirectCommand record{.X = 8, .Y = 4, .Z = 6};
    CHECK(Word(record, 0) == 8);
    CHECK(Word(record, 1) == 4);
    CHECK(Word(record, 2) == 6);
}
