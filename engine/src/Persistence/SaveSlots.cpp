#include <Veng/Persistence/SaveSlots.h>

#include <Veng/Persistence/LocalAccountStore.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <utility>

namespace Veng
{
    namespace
    {
        // Whether a byte is whitespace the slot-name normalizer folds.
        [[nodiscard]] bool IsSlotNameSpace(const char c)
        {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        // Whether a byte survives normalization: control codes and the separators/wildcards no path
        // component may carry on any supported platform are dropped.
        [[nodiscard]] bool IsSlotNamePrintable(const char c)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7F)
            {
                return false;
            }
            constexpr string_view Forbidden = "/\\:*?\"<>|";
            return Forbidden.find(c) == string_view::npos;
        }

        [[nodiscard]] char ToLowerAscii(const char c)
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        }

        // Whether a normalized name collides with a Windows device name. Windows resolves these
        // before the filesystem and with any extension attached, so `CON` and `con.save` both name
        // the console rather than a directory — a slot named either is unopenable there, and the
        // rule is applied on every platform so a slot list is not platform-dependent.
        [[nodiscard]] bool IsReservedDeviceName(const string& normalized)
        {
            const usize dot = normalized.find('.');
            string stem = normalized.substr(0, dot == string::npos ? normalized.size() : dot);
            while (!stem.empty() && IsSlotNameSpace(stem.back()))
            {
                stem.pop_back();
            }
            std::ranges::transform(stem, stem.begin(), ToLowerAscii);

            constexpr std::array<string_view, 4> Devices{"con", "prn", "aux", "nul"};
            if (std::ranges::find(Devices, stem) != Devices.end())
            {
                return true;
            }
            // COM0-9 and LPT0-9, the numbered serial and printer ports.
            if (stem.size() == 4 && stem.starts_with("com") && stem[3] >= '0' && stem[3] <= '9')
            {
                return true;
            }
            return stem.size() == 4 && stem.starts_with("lpt") && stem[3] >= '0' && stem[3] <= '9';
        }

        // Whether a normalized name would resolve onto the local account store's files. A consumer
        // that roots the account record and its slots together holds `account`, `account.lock`, and
        // `account.corrupt` in that directory, and a slot of any of those names would have the
        // store create a directory over a regular file. The whole stem is reserved rather than the
        // bare name, matched case-insensitively because the platforms that fold case would resolve
        // `Account` onto the same file.
        [[nodiscard]] bool IsReservedAccountName(const string& normalized)
        {
            const usize dot = normalized.find('.');
            string stem = normalized.substr(0, dot == string::npos ? normalized.size() : dot);
            std::ranges::transform(stem, stem.begin(), ToLowerAscii);
            return stem == LocalAccountStore::FileName;
        }

        // The directory's last-write time as whole Unix-epoch seconds; 0 when it cannot be read.
        [[nodiscard]] i64 LastWriteSeconds(const path& directory)
        {
            std::error_code ec;
            const std::filesystem::file_time_type written =
                std::filesystem::last_write_time(directory, ec);
            if (ec)
            {
                return 0;
            }
            // file_clock's epoch is unspecified and an implementation supplies either to_sys or
            // to_utc, not both, so neither spelling is portable; clock_cast bridges them but
            // reaches the leap-second table through the timezone database, which is a throwing
            // runtime dependency. Offsetting by the two clocks' current readings names neither
            // conversion, at the cost of the microseconds between the two now() calls — below the
            // second this truncates to, and the stamp is advisory ordering rather than a timestamp.
            const auto systemTime = std::chrono::system_clock::now() +
                                    (written - std::filesystem::file_time_type::clock::now());
            const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(systemTime);
            return static_cast<i64>(seconds.time_since_epoch().count());
        }
    }

    string NormalizeSlotName(const string_view raw)
    {
        string folded;
        folded.reserve(raw.size());
        bool pendingSpace = false;
        for (const char c : raw)
        {
            if (IsSlotNameSpace(c))
            {
                // A whitespace run collapses to one space, and a leading run is dropped entirely.
                pendingSpace = !folded.empty();
                continue;
            }
            if (!IsSlotNamePrintable(c))
            {
                continue;
            }
            if (pendingSpace)
            {
                folded.push_back(' ');
                pendingSpace = false;
            }
            folded.push_back(c);
        }
        if (folded.size() > MaxSlotNameLength)
        {
            folded.resize(MaxSlotNameLength);
        }
        // The truncation above can expose a trailing space the collapse had put mid-name.
        while (!folded.empty() && IsSlotNameSpace(folded.back()))
        {
            folded.pop_back();
        }
        return folded;
    }

    bool IsValidSlotName(const string_view raw)
    {
        const string normalized = NormalizeSlotName(raw);
        if (normalized.empty() || normalized == "." || normalized == "..")
        {
            return false;
        }
        // Windows strips a trailing dot, so such a name never round-trips to the directory it
        // created.
        if (normalized.back() == '.')
        {
            return false;
        }
        return !IsReservedDeviceName(normalized) && !IsReservedAccountName(normalized);
    }

    Result<path> SlotDirectoryOf(const path& root, const string_view name)
    {
        if (root.empty())
        {
            return std::unexpected(string{"no slot root is configured"});
        }
        if (!IsValidSlotName(name))
        {
            return std::unexpected(fmt::format("'{}' is not a usable slot name", name));
        }
        return root / NormalizeSlotName(name);
    }

    vector<SlotInfo> EnumerateSlots(const path& root)
    {
        vector<SlotInfo> slots;
        std::error_code ec;
        if (root.empty() || !std::filesystem::is_directory(root, ec))
        {
            return slots;
        }
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(root, ec))
        {
            if (!entry.is_directory(ec))
            {
                continue;
            }
            SlotInfo slot{.Name = entry.path().filename().string(),
                          .Directory = entry.path(),
                          .LastWriteWall = LastWriteSeconds(entry.path())};
            slots.push_back(std::move(slot));
        }
        // Name breaks the tie: the stamp has one-second resolution, so slots written within the same
        // second would otherwise enumerate in directory order, which is not stable.
        std::ranges::sort(slots,
                          [](const SlotInfo& a, const SlotInfo& b)
                          {
                              if (a.LastWriteWall != b.LastWriteWall)
                              {
                                  return a.LastWriteWall > b.LastWriteWall;
                              }
                              return a.Name < b.Name;
                          });
        return slots;
    }

    Result<Unique<Store>> OpenSlot(const path& root, const string_view name,
                                   const bool createIfAbsent)
    {
        const Result<path> directory = SlotDirectoryOf(root, name);
        if (!directory)
        {
            return std::unexpected(directory.error());
        }
        std::error_code ec;
        if (!createIfAbsent && !std::filesystem::is_directory(*directory, ec))
        {
            return std::unexpected(fmt::format("no slot named '{}'", name));
        }
        return Store::Open(*directory);
    }
}
