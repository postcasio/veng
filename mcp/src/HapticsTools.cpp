#include "HapticsTools.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Haptics/Haptics.h>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief A pad slot as a number, or null for GamepadId::None.
        Json PadOrNull(const GamepadId pad)
        {
            if (pad == GamepadId::None)
            {
                return nullptr;
            }
            return static_cast<u32>(pad);
        }

        /// @brief The four channels as a JSON object.
        Json ChannelsJson(const Haptics::RumbleChannels& channels)
        {
            return Json{{"low_frequency", channels.LowFrequency},
                        {"high_frequency", channels.HighFrequency},
                        {"left_trigger", channels.LeftTrigger},
                        {"right_trigger", channels.RightTrigger}};
        }

        /// @brief An instance's target as a JSON object.
        Json TargetJson(const Haptics::RumbleTarget& target)
        {
            switch (target.Kind)
            {
            case Haptics::RumbleTargetKind::Seat:
            {
                Json viewer = nullptr;
                if (!target.Seat.IsImplicit())
                {
                    viewer = Json{{"index", target.Seat.Viewer.Index},
                                  {"generation", target.Seat.Viewer.Generation}};
                }
                return Json{{"kind", "seat"},
                            {"world", target.Seat.World.Value},
                            {"viewer", std::move(viewer)}};
            }
            case Haptics::RumbleTargetKind::Gamepad:
                return Json{{"kind", "gamepad"}, {"pad", PadOrNull(target.Gamepad)}};
            case Haptics::RumbleTargetKind::None:
                break;
            }
            return Json{{"kind", "none"}};
        }
    }

    void RegisterHapticsTools(McpServer& server, const McpHost& host)
    {
        // haptics.state — the rumble mix: what each pad's motors are driven at and every instance
        // feeding them. Read-only, so it is always registered.
        McpTool tool;
        tool.Name = "haptics.state";
        tool.Description =
            "Reports the haptics engine's rumble state: the master intensity, whether the device "
            "output is suspended (the window is unfocused), each pad slot that is playing or "
            "driven (its mixed low_frequency/high_frequency/left_trigger/right_trigger levels, "
            "0..1, "
            "before suspension), and every live rumble instance — its handle, target (a seat or a "
            "pad), the pad it resolved to, clip (a hex AssetId), time, duration, intensity, fade, "
            "loop, stopping and paused flags, and owning world (null when application-owned). "
            "Works on virtual pads, so a driven session verifies rumble without hardware. Takes no "
            "arguments.";
        tool.InputSchemaJson = R"({"type":"object","properties":{}})";
        tool.Handler = [&host](string_view) -> Result<string>
        {
            Haptics::HapticsEngine* const engine = host.Haptics ? host.Haptics() : nullptr;
            if (engine == nullptr)
            {
                return std::unexpected(
                    string("haptics is unavailable: this host exposes no haptics engine"));
            }

            const vector<Haptics::RumbleInstanceInfo> live = engine->GetAllInstances();

            Json pads = Json::array();
            for (u32 slot = 0; slot < Input::MaxGamepads; ++slot)
            {
                const auto pad = static_cast<GamepadId>(slot);
                const Haptics::RumbleChannels output = engine->GetOutput(pad);
                const auto playing = static_cast<usize>(
                    std::ranges::count_if(live, [pad](const Haptics::RumbleInstanceInfo& i)
                                          { return i.Gamepad == pad; }));
                if (playing == 0 && output == Haptics::RumbleChannels{})
                {
                    continue;
                }
                pads.push_back(Json{
                    {"pad", slot}, {"instance_count", playing}, {"output", ChannelsJson(output)}});
            }

            Json instances = Json::array();
            for (const Haptics::RumbleInstanceInfo& info : live)
            {
                instances.push_back(Json{
                    {"slot", info.Handle.Slot},
                    {"generation", info.Handle.Generation},
                    {"target", TargetJson(info.Target)},
                    {"pad", PadOrNull(info.Gamepad)},
                    {"clip", fmt::format("0x{:016X}", info.Clip.Value)},
                    {"time", info.Time},
                    {"duration", info.Duration},
                    {"intensity", info.Intensity},
                    {"fade", info.Fade},
                    {"loop", info.Loop},
                    {"stopping", info.Stopping},
                    {"paused", info.Paused},
                    {"world", info.World.IsValid() ? Json(info.World.Value) : Json(nullptr)},
                });
            }

            return Json{{"master_intensity", engine->GetMasterIntensity()},
                        {"suspended", engine->IsOutputSuspended()},
                        {"pads", std::move(pads)},
                        {"instances", std::move(instances)}}
                .dump();
        };
        server.RegisterTool(std::move(tool));
    }
}
