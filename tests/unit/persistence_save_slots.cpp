// Save-slot tests: the slot-name normalization rules (whitespace folding, dropped characters, the
// length cap, collision by normalization), the validation rules that keep a name a single path
// component inside its root (`.`/`..`, separators, platform-reserved device names), slot
// enumeration over a root that also holds stray files, and the open/create and contention paths
// through OpenSlot.

#include <doctest/doctest.h>

#include <Veng/Persistence/SaveSlots.h>

#include <support/TempPath.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace Veng;

namespace
{
    // A fresh, unique slot root per case under the process's scratch tree, removed on destruction.
    struct TempRoot
    {
        path Dir;

        TempRoot()
        {
            static std::atomic<u64> counter{0};
            Dir = TestSupport::TempDir() /
                  fmt::format("slots-{}", counter.fetch_add(1, std::memory_order_relaxed));
            std::filesystem::remove_all(Dir);
            std::filesystem::create_directories(Dir);
        }

        ~TempRoot() { std::filesystem::remove_all(Dir); }
    };

    void TouchFile(const path& file)
    {
        std::ofstream stream(file, std::ios::binary);
        stream << "not a slot";
    }
}

TEST_CASE("slot names normalize by trimming, collapsing, and dropping")
{
    CHECK(NormalizeSlotName("  Deep Space  ") == "Deep Space");
    CHECK(NormalizeSlotName("Deep\t\r\n  Space") == "Deep Space");
    CHECK(NormalizeSlotName("a/b\\c:d*e?f\"g<h>i|j") == "abcdefghij");
    CHECK(NormalizeSlotName("bell\x07"
                            "end") == "bellend");
    CHECK(NormalizeSlotName("") == "");
    CHECK(NormalizeSlotName("   ") == "");

    // Case is preserved: folding would merge names differing only in case and rename an existing
    // slot's directory on the first reopen.
    CHECK(NormalizeSlotName("Alpha") == "Alpha");
    CHECK(NormalizeSlotName("alpha") == "alpha");
}

TEST_CASE("slot names truncate to the length cap without a trailing space")
{
    const string overlong(MaxSlotNameLength + 20, 'x');
    CHECK(NormalizeSlotName(overlong).size() == MaxSlotNameLength);

    // The cut lands right after a space, which the trailing trim then removes.
    string cutAtSpace(MaxSlotNameLength - 1, 'y');
    cutAtSpace += " zzz";
    const string normalized = NormalizeSlotName(cutAtSpace);
    CHECK(normalized == string(MaxSlotNameLength - 1, 'y'));
}

TEST_CASE("distinct raw names can normalize onto the same slot")
{
    // Normalization is a mapping, so two raw names may denote one slot — the store then treats them
    // as the same directory, which is the intended behaviour and not a collision to detect.
    const TempRoot root;
    const Result<path> a = SlotDirectoryOf(root.Dir, "  Deep   Space ");
    const Result<path> b = SlotDirectoryOf(root.Dir, "Deep Space");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a == *b);
}

TEST_CASE("slot name validation rejects the unusable")
{
    CHECK(IsValidSlotName("Deep Space"));
    CHECK(IsValidSlotName("save-01"));
    CHECK(IsValidSlotName("save.01"));

    CHECK_FALSE(IsValidSlotName(""));
    CHECK_FALSE(IsValidSlotName("   "));
    CHECK_FALSE(IsValidSlotName("."));
    CHECK_FALSE(IsValidSlotName(".."));
    CHECK_FALSE(IsValidSlotName("  ..  "));
    CHECK_FALSE(IsValidSlotName("trailing."));

    // The Windows device names, matched case-insensitively and with any extension.
    CHECK_FALSE(IsValidSlotName("CON"));
    CHECK_FALSE(IsValidSlotName("nul"));
    CHECK_FALSE(IsValidSlotName("CoM1"));
    CHECK_FALSE(IsValidSlotName("lpt9.save"));
    CHECK(IsValidSlotName("console"));
    CHECK(IsValidSlotName("com10"));
}

TEST_CASE("slot resolution refuses traversal and always stays one component under the root")
{
    const TempRoot root;

    CHECK_FALSE(SlotDirectoryOf(root.Dir, ".").has_value());
    CHECK_FALSE(SlotDirectoryOf(root.Dir, "..").has_value());
    CHECK_FALSE(SlotDirectoryOf(root.Dir, "../..").has_value());
    CHECK_FALSE(SlotDirectoryOf(root.Dir, "NUL").has_value());
    CHECK_FALSE(SlotDirectoryOf(root.Dir, "").has_value());
    CHECK_FALSE(SlotDirectoryOf(path{}, "any").has_value());

    // A name carrying separators loses them to normalization rather than resolving to a nested or
    // parent directory: whatever survives is one component directly under the root.
    for (const string_view name : {"a/../b", "..\\..\\etc", "/etc/passwd", "sub/dir"})
    {
        const Result<path> resolved = SlotDirectoryOf(root.Dir, name);
        REQUIRE(resolved.has_value());
        CHECK(resolved->parent_path() == root.Dir);
        CHECK(resolved->filename() == NormalizeSlotName(name));
        CHECK(std::filesystem::path(NormalizeSlotName(name)).has_parent_path() == false);
    }
}

TEST_CASE("enumeration lists slot directories, skips stray files, and orders newest first")
{
    const TempRoot root;
    CHECK(EnumerateSlots(root.Dir).empty());
    CHECK(EnumerateSlots(root.Dir / "absent").empty());
    CHECK(EnumerateSlots(path{}).empty());

    std::filesystem::create_directories(root.Dir / "alpha");
    std::filesystem::create_directories(root.Dir / "beta");
    // A consumer's own file beside the slots — an account record, say — is not a slot.
    TouchFile(root.Dir / "account");

    vector<SlotInfo> slots = EnumerateSlots(root.Dir);
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].Name == "alpha");
    CHECK(slots[1].Name == "beta");
    CHECK(slots[0].Directory == root.Dir / "alpha");
    CHECK(slots[0].LastWriteWall > 0);

    // Newest first: touching beta's directory content moves it ahead of alpha.
    const auto later = std::filesystem::file_time_type::clock::now() + std::chrono::hours(1);
    std::error_code ec;
    std::filesystem::last_write_time(root.Dir / "beta", later, ec);
    REQUIRE_FALSE(ec);

    slots = EnumerateSlots(root.Dir);
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].Name == "beta");
    CHECK(slots[1].Name == "alpha");
    CHECK(slots[0].LastWriteWall > slots[1].LastWriteWall);
}

TEST_CASE("opening a slot creates it only when asked")
{
    const TempRoot root;

    const Result<Unique<Store>> absent = OpenSlot(root.Dir, "fresh", false);
    CHECK_FALSE(absent.has_value());
    CHECK_FALSE(std::filesystem::exists(root.Dir / "fresh"));

    {
        Result<Unique<Store>> created = OpenSlot(root.Dir, "fresh", true);
        REQUIRE(created.has_value());
        CHECK((*created)->GetSlotDirectory() == root.Dir / "fresh");
        CHECK(std::filesystem::is_directory(root.Dir / "fresh"));
    }

    // Now it exists, so the no-create open succeeds.
    const Result<Unique<Store>> reopened = OpenSlot(root.Dir, "fresh", false);
    CHECK(reopened.has_value());

    // An unusable name never reaches the store, so nothing is created for it.
    CHECK_FALSE(OpenSlot(root.Dir, "..", true).has_value());
    CHECK_FALSE(std::filesystem::exists(root.Dir / "slot.lock"));
}

TEST_CASE("a contended slot surfaces the store's reason through OpenSlot")
{
    const TempRoot root;
    const Result<Unique<Store>> held = OpenSlot(root.Dir, "busy", true);
    REQUIRE(held.has_value());

    const Result<Unique<Store>> second = OpenSlot(root.Dir, "busy", true);
    REQUIRE_FALSE(second.has_value());
    CHECK_FALSE(second.error().empty());
}
