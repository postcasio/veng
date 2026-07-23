#include <Veng/Asset/AtomicFile.h>
#include <Veng/Asset/Path.h>

#include <fstream>
#include <random>
#include <system_error>

#include <fmt/format.h>

namespace Veng
{
    VoidResult WriteFileAtomic(const path& filePath, std::span<const u8> bytes)
    {
        // The temporary lives beside the destination so the rename never crosses a
        // filesystem; the random suffix keeps concurrent writers of the same output
        // off each other's temporary.
        std::random_device rng;
        path temp = filePath;
        temp += fmt::format(".{:08x}.tmp", rng());

        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                return std::unexpected(
                    fmt::format("WriteFileAtomic: failed to open '{}' for writing", temp.string()));
            }

            if (!bytes.empty())
            {
                file.write(reinterpret_cast<const char*>(bytes.data()),
                           static_cast<std::streamsize>(bytes.size()));
            }

            file.flush();
            if (!file)
            {
                std::error_code discard;
                std::filesystem::remove(temp, discard);
                return std::unexpected(
                    fmt::format("WriteFileAtomic: failed writing to '{}'", temp.string()));
            }
        }

        std::error_code ec;
        std::filesystem::rename(temp, filePath, ec);
        if (ec)
        {
            std::error_code discard;
            std::filesystem::remove(temp, discard);
            return std::unexpected(fmt::format("WriteFileAtomic: failed to rename '{}' -> '{}': {}",
                                               temp.string(), filePath.string(), ec.message()));
        }

        return {};
    }
}
