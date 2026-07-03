#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Asset/AssetId.h>

#include <span>

namespace Veng
{
    /// @brief Parsed launcher command-line arguments the engine acts on before running.
    ///
    /// The launcher forwards its raw argv into Application::Run, which parses it into this
    /// struct once via Parse and stores it (Application::GetLaunchArguments). The engine
    /// consumes the recognised options itself — a game subclass wires nothing to get them,
    /// and reads this only if it wants to.
    struct LaunchArguments
    {
        /// @brief Working directory to switch to before running; unset leaves it unchanged.
        ///
        /// The first positional (non-flag) argument, matching the launcher's long-standing
        /// convention. Parse does not touch the filesystem — Run validates and applies it.
        optional<path> WorkingDirectory;

        /// @brief Startup-level override; unset uses the cooked project's StartupLevel.
        ///
        /// Set by `--level=<id>` (or `--level <id>`), where `<id>` is an AssetId written in
        /// decimal or `0x`-prefixed hex. It selects among the levels cooked into the project's
        /// mounted packs, not an arbitrary external file; an id absent from those packs fails
        /// the load like any missing asset.
        optional<AssetId> Level;

        /// @brief Parses launcher arguments (argv without the program name) into a LaunchArguments.
        ///
        /// Pure and device-free — it reads no files and touches no global state, so it is unit
        /// tested with no window. Recognises `--level=<id>` / `--level <id>` and one leading
        /// positional working directory.
        /// @param args  The argument tokens after argv[0].
        /// @return The parsed arguments, or an error string for an unknown flag, a malformed
        ///         level id, or a second positional argument.
        [[nodiscard]] static VE_API Result<LaunchArguments> Parse(std::span<const string> args);
    };
}
