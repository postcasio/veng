// Cooked-record padding tests: the importers memcpy a whole Cooked* record into the blob, so a
// value-initialized record must have no indeterminate padding — an unnamed alignment gap would
// leak stack bytes into the cooked blob and into the content hash the cook cache keys on, so a
// cooked pack would not reproduce across builds. The property here is that a value-initialized
// record fully determines every byte of its object representation.

#include <cstring>
#include <new>

#include <doctest/doctest.h>

#include <Veng/Asset/CookedBlobs.h>

using namespace Veng;

namespace
{
    // Value-initialize a T{} over storage poisoned 0x00 and again over storage poisoned 0xFF, and
    // return the first byte that differs (or -1 when every byte is determined). A differing byte is
    // padding the record does not initialize and would leak into a memcpy'd blob.
    template <class T>
    int FirstIndeterminateByte()
    {
        alignas(T) unsigned char zeros[sizeof(T)];
        alignas(T) unsigned char ones[sizeof(T)];
        std::memset(zeros, 0x00, sizeof(T));
        std::memset(ones, 0xFF, sizeof(T));
        new (zeros) T{};
        new (ones) T{};
        for (usize i = 0; i < sizeof(T); ++i)
        {
            if (zeros[i] != ones[i])
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
}

TEST_CASE("cooked blob padding: a value-initialized record leaks no indeterminate padding")
{
    // CookedLevelHeader carries a u64 after a u32, forcing an interior gap plus tail padding;
    // CookedUIElement is 8-byte-aligned for its u64 members and leaves tail padding after its
    // trailing u32. Both are memcpy'd whole into the blob, so both gaps must be named and zeroed.
    CHECK(FirstIndeterminateByte<CookedLevelHeader>() == -1);
    CHECK(FirstIndeterminateByte<CookedUIElement>() == -1);
}
