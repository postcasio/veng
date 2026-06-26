#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/Types.h>

// The .vengproj cooked-project file — the runtime entrypoint of a managed game:
//
//   Header
//     char  magic[8]      "VENGPROJ"
//     u32   version
//     u32   packCount     // number of pack mount names
//     u64   startupLevel  // AssetId of the level the engine bootstraps, 0 = none
//   PackNames[packCount]
//     u32   length        // byte length of the UTF-8 mount name
//     char  bytes[length] // the pack's file name, mounted from the executable directory
//
// The cook resolves project.veng (the JSON authoring file) into one .vengproj per build
// configuration; the runtime reads it, mounts each named pack in order, and loads the
// startup level. The format assumes the cook host and run host share endianness.

namespace Veng
{
    /// @brief The cooked-project format version written and checked by the read/write helpers.
    ///
    /// A mismatch produces a clean error, not a crash. Bump on any layout change.
    inline constexpr u32 CookedProjectFormatVersion = 1;

    /// @brief The runtime projection of a project for one build configuration.
    ///
    /// Names the packs to mount (by file name, resolved beside the executable) and the level
    /// the engine bootstraps. The cooker writes it from the project's settings; the runtime
    /// reads it back to mount the world.
    struct CookedProject
    {
        /// @brief The level the engine loads after mounting the packs; the invalid id (0) means none.
        AssetId StartupLevel;
        /// @brief The pack file names to mount, in mount order (e.g. "template.vengpack").
        vector<string> PackMountNames;
    };

    /// @brief Reads a .vengproj file into a CookedProject.
    ///
    /// Rejects a bad magic or a version mismatch with a located error string.
    /// @param filePath  Path to the .vengproj file.
    /// @return The parsed project on success, or an error string on failure.
    [[nodiscard]] Result<CookedProject> ReadCookedProject(const path& filePath);

    /// @brief Serializes a CookedProject to a .vengproj file.
    /// @param filePath  Destination path; the file is truncated if it already exists.
    /// @param project   The project to write.
    /// @return An error string on failure.
    [[nodiscard]] VoidResult WriteCookedProject(const path& filePath, const CookedProject& project);
}
