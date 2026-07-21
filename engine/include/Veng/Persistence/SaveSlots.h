#pragma once

#include <Veng/Persistence/Store.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

namespace Veng
{
    /// @brief The longest slot name accepted, in characters.
    inline constexpr usize MaxSlotNameLength = 32;

    /// @brief Normalizes a raw slot name into the directory name it is stored under.
    ///
    /// Trims, collapses internal whitespace runs to one space, drops control characters and the
    /// characters no path component may carry (`/ \ : * ? " < > |`), and truncates to
    /// MaxSlotNameLength. Case is preserved: folding it would silently merge names differing only
    /// in case, and rename an existing slot's directory the first time it was reopened.
    ///
    /// Normalization maps; it never rejects. A name that normalizes to nothing, to a relative-path
    /// element, or to a platform-reserved name is refused by IsValidSlotName.
    /// @param raw  The raw text a consumer supplies (typed by a user, or read off a command line).
    /// @return The normalized name, possibly empty.
    [[nodiscard]] VE_API string NormalizeSlotName(string_view raw);

    /// @brief Returns whether a name is usable as a slot's directory name.
    ///
    /// Normalizes @p raw, then rejects an empty result, the relative-path elements `.` and `..`, a
    /// name ending in a dot, the platform-reserved device names (`CON`, `NUL`, `COM1`, …, matched
    /// case-insensitively and with any extension), and `LocalAccountStore::FileName` with any
    /// extension, so a slot cannot resolve onto the account record where a consumer roots the two
    /// together. The separators and wildcards are gone by normalization, so an accepted name is
    /// always a single path component.
    /// @param raw  The name to test, normalized or not.
    [[nodiscard]] VE_API bool IsValidSlotName(string_view raw);

    /// @brief One slot under a root: its name, its directory, and when the directory last changed.
    struct SlotInfo
    {
        /// @brief The slot's name — its directory name under the root.
        string Name;
        /// @brief The slot's directory.
        path Directory;
        /// @brief Wall-clock seconds (Unix epoch) of the directory's last write; 0 when unreadable.
        ///
        /// A filesystem fact, not a semantic "when was this slot last saved": Store::Open writes
        /// the slot lock and may sweep superseded files, so opening a slot perturbs it. A consumer
        /// ordering a user-facing list on when the state was last written must key on its own
        /// persisted stamp.
        i64 LastWriteWall = 0;
    };

    /// @brief The directory a named slot lives in under a root.
    ///
    /// Returns a Result rather than a bare path because the invalid case has no safe answer: the
    /// only path a rejected name could resolve to is @p root itself, which OpenSlot would then lock
    /// and sweep as though the whole root were one slot.
    ///
    /// The root is supplied by the caller — these helpers resolve nothing global.
    /// `Platform/UserPaths.h`'s UserDataDir() is the natural provider of a per-user root, but it is
    /// not required: a portable application may root beside its executable, and a test roots in a
    /// temporary directory.
    /// @param root  The directory the slots live under; any intermediate segment in a consumer's
    /// layout belongs to its own root resolution, not here.
    /// @param name  The slot's name; normalized here.
    /// @return The slot's directory, or the reason the name is unusable.
    [[nodiscard]] VE_API Result<path> SlotDirectoryOf(const path& root, string_view name);

    /// @brief Lists every slot directory under a root, most recently written first.
    ///
    /// Reports filesystem facts only and opens no store: a slot's display metadata is the
    /// consumer's, layered through its own store family. Non-directories are skipped, so a root
    /// holding consumer files beside its slots enumerates cleanly.
    /// @param root  The directory the slots live under; a missing root lists nothing.
    /// @return The slots, ordered by LastWriteWall descending then by name.
    [[nodiscard]] VE_API vector<SlotInfo> EnumerateSlots(const path& root);

    /// @brief Opens a named slot's store, optionally creating the slot when it does not exist yet.
    ///
    /// The entry point that turns a name into an open store. Families are registered by the caller,
    /// as on any store.
    /// @param root            The directory the slots live under.
    /// @param name            The slot's name; normalized here.
    /// @param createIfAbsent  Whether a missing slot is created rather than an error.
    /// @return The opened store, or a recoverable error — an unusable name, an absent slot without
    /// @p createIfAbsent, or whatever Store::Open reports (lock contention, an unreadable or
    /// unrecognized slot).
    [[nodiscard]] VE_API Result<Unique<Store>> OpenSlot(const path& root, string_view name,
                                                        bool createIfAbsent);
}
