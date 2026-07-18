#pragma once

#include <Veng/Veng.h>
#include <Veng/Result.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Net/FaultInjectionTransport.h>

#include <span>

namespace Veng
{
    /// @brief Parsed launcher command-line arguments the engine acts on before running.
    ///
    /// The launcher forwards its raw argv into Application::Run, which parses it into this
    /// struct once via Parse and stores it (Application::GetLaunchArguments). The engine
    /// consumes the recognised options itself — a game subclass wires nothing to get them,
    /// and reads this only if it wants to.
    /// @brief A resolved `--join` target: the server host to connect to and an optional port.
    struct JoinTarget
    {
        /// @brief The server host to resolve (an address literal for UDP).
        string Host;
        /// @brief The server port, or 0 to use the app's configured default (GameNetInfo::Port).
        u16 Port = 0;
    };

    /// @brief A command-line option an application accepts, beyond the engine's own flags.
    ///
    /// Declared through ApplicationInfo::LaunchOptions and consumed by Parse into
    /// LaunchArguments::GameOptions. An engine flag of the same name always wins.
    struct LaunchOptionInfo
    {
        /// @brief The option's long name, without the leading `--`.
        string Name;
        /// @brief Whether the option consumes a value (`--opt <v>` or `--opt=<v>`) rather than standing alone.
        bool TakesValue = true;
    };

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

        /// @brief Start listening as a server (`--server`), hosting the managed world.
        ///
        /// The listen/dedicated activation: the engine opens a ServerHost on the managed world and
        /// accepts connections. Combined with Headless it is a dedicated server (no render tail);
        /// windowed it is a listen server (the same code plus local seats and a screen). A game that
        /// sets no ApplicationInfo::Net still gets defaults here — zero-config LAN hosting.
        bool Server = false;

        /// @brief Run without a window (`--headless`), forcing ApplicationInfo::Headless on.
        ///
        /// Applied to the app's ApplicationInfo before initialization, so `--server --headless` boots
        /// the dedicated server (accumulator + net pump, no View/render tail) from a windowed game exe.
        bool Headless = false;

        /// @brief Boot a dedicated server (`--dedicated`): a headless listen host with no local seat.
        ///
        /// The first-class dedicated-server flag and the honest name for the `--server --headless`
        /// composite: it sets both Server and Headless, so the process opens a ServerHost on the managed
        /// world and runs the accumulator + net pump with no window, viewport, or local presenter. A
        /// dedicated host differs from a listen host only in the absence of that local seat and screen.
        bool Dedicated = false;

        /// @brief The local player's name (`--name <s>`); unset means anonymous.
        ///
        /// An opaque launch token the engine itself never consumes: a game reads it back
        /// (GetLaunchArguments) — typically to derive a stable account identity through
        /// GameNetInfo::Identity, so relaunching with the same name reattaches as the same player.
        optional<string> Name;

        /// @brief Connect to a server as a client (`--join <host[:port]>`); unset stays standalone/server.
        ///
        /// The client activation: the engine connects a Net::Client to the target and drives the world
        /// in client mode (NetRole::Client), the level arriving from the accept payload. An omitted
        /// port uses the app's configured default.
        optional<JoinTarget> Join;

        /// @brief Network simulation to wrap the transport with (`--netsim latency=100,loss=5,...`); unset is a clean link.
        ///
        /// A dev/QA adversity tool shipped in every build: it wraps whichever transport the mode
        /// constructs (server or client) in a SimulatedTransport with seeded loss/dup/reorder + a
        /// latency/jitter delay, so the exemplar is playable under simulated adversity. The flag's
        /// loss/dup/reorder are percentages (mapped to [0,1] rates), latency/jitter milliseconds.
        optional<Net::FaultInjectionConfig> NetSim;

        /// @brief Values for the application-declared launch options present on the command line.
        ///
        /// Keyed by option name without the leading `--`. A declared value-less option that appeared
        /// maps to an empty string; a declared option absent from the command line has no entry.
        /// Read back through Application::GetLaunchArguments().
        map<string, string> GameOptions;

        /// @brief Parses launcher arguments (argv without the program name) into a LaunchArguments.
        ///
        /// Pure and device-free — it reads no files and touches no global state, so it is unit
        /// tested with no window. Recognises `--level=<id>` / `--level <id>`, `--server`,
        /// `--headless`, `--dedicated` (`--server --headless`), `--join <host[:port]>`,
        /// `--name <s>`, one leading positional working directory, and each option in `options`.
        /// A `--` token matching neither an engine flag nor a declared option is an error, so a
        /// misspelled flag is caught rather than ignored.
        /// @param args     The argument tokens after argv[0].
        /// @param options  The application's declared options, consumed into GameOptions. Empty
        ///                 (the default) recognises the engine flags alone.
        /// @return The parsed arguments, or an error string for an unknown flag, a declared option
        ///         missing its value, a malformed level id, a malformed join target, or a second
        ///         positional argument.
        [[nodiscard]] static VE_API Result<LaunchArguments>
        Parse(std::span<const string> args, std::span<const LaunchOptionInfo> options = {});
    };
}
