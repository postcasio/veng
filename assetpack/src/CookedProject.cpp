#include <Veng/Asset/CookedProject.h>

#include <cstring>
#include <fstream>

#include <fmt/format.h>

namespace Veng
{
    namespace
    {
        constexpr char CookedProjectMagic[8] = {'V', 'E', 'N', 'G', 'P', 'R', 'O', 'J'};

        // Fixed-size leading header; the variable-length pack names follow it. Field order keeps
        // the u64 8-byte-aligned so the struct packs to 24 bytes with no padding.
        struct OnDiskHeader
        {
            char Magic[8];
            u32 Version;
            u32 PackCount;
            u64 StartupLevel; // AssetId, 0 = none
        };
        static_assert(sizeof(OnDiskHeader) == 24);
    }

    Result<CookedProject> ReadCookedProject(const path& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        if (!file)
        {
            return std::unexpected(
                fmt::format("ReadCookedProject: failed to open '{}'", filePath.string()));
        }

        std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());

        if (bytes.size() < sizeof(OnDiskHeader))
        {
            return std::unexpected(fmt::format(
                "ReadCookedProject: '{}' too small for the project header", filePath.string()));
        }

        OnDiskHeader header;
        std::memcpy(&header, bytes.data(), sizeof(header));

        if (std::memcmp(header.Magic, CookedProjectMagic, sizeof(CookedProjectMagic)) != 0)
        {
            return std::unexpected(fmt::format(
                "ReadCookedProject: '{}' has a bad magic (not a .vengproj)", filePath.string()));
        }

        if (header.Version != CookedProjectFormatVersion)
        {
            return std::unexpected(
                fmt::format("ReadCookedProject: '{}' version mismatch (expected {}, got {})",
                            filePath.string(), CookedProjectFormatVersion, header.Version));
        }

        CookedProject project;
        project.StartupLevel = AssetId{.Value = header.StartupLevel};
        project.PackMountNames.reserve(header.PackCount);

        usize cursor = sizeof(OnDiskHeader);
        for (u32 i = 0; i < header.PackCount; ++i)
        {
            if (cursor + sizeof(u32) > bytes.size())
            {
                return std::unexpected(
                    fmt::format("ReadCookedProject: '{}' truncated reading pack name length",
                                filePath.string()));
            }

            u32 length = 0;
            std::memcpy(&length, bytes.data() + cursor, sizeof(length));
            cursor += sizeof(length);

            if (cursor + length > bytes.size())
            {
                return std::unexpected(
                    fmt::format("ReadCookedProject: '{}' truncated reading pack name bytes",
                                filePath.string()));
            }

            project.PackMountNames.emplace_back(bytes.data() + cursor, length);
            cursor += length;
        }

        return project;
    }

    VoidResult WriteCookedProject(const path& filePath, const CookedProject& project)
    {
        OnDiskHeader header{};
        std::memcpy(header.Magic, CookedProjectMagic, sizeof(CookedProjectMagic));
        header.Version = CookedProjectFormatVersion;
        header.StartupLevel = project.StartupLevel.Value;
        header.PackCount = static_cast<u32>(project.PackMountNames.size());

        vector<u8> out(sizeof(OnDiskHeader));
        std::memcpy(out.data(), &header, sizeof(header));

        for (const string& name : project.PackMountNames)
        {
            const auto length = static_cast<u32>(name.size());
            const usize cursor = out.size();
            out.resize(cursor + sizeof(length) + name.size());
            std::memcpy(out.data() + cursor, &length, sizeof(length));
            std::memcpy(out.data() + cursor + sizeof(length), name.data(), name.size());
        }

        std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            return std::unexpected(fmt::format(
                "WriteCookedProject: failed to open '{}' for writing", filePath.string()));
        }

        file.write(reinterpret_cast<const char*>(out.data()),
                   static_cast<std::streamsize>(out.size()));
        if (!file)
        {
            return std::unexpected(
                fmt::format("WriteCookedProject: failed writing to '{}'", filePath.string()));
        }

        return {};
    }
}
