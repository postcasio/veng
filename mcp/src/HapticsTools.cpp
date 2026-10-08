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

        /// @brief A presentation state's name.
        const char* StateName(const PresentationState state)
        {
            switch (state)
            {
            case PresentationState::Live:
                return "live";
            case PresentationState::Muted:
                return "muted";
            case PresentationState::Held:
                return "held";
            case PresentationState::Closed:
                break;
            }
            return "closed";
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
            "0..1, before suspension, and how many one-shots and layers feed it), every one-shot "
            "held (its owning presentation scope and that scope's state — live, muted, held or "
            "closed — the pad it plays on, clip as a hex AssetId, time, duration and intensity), "
            "and "
            "every layer the last frame mixed (its scope, state, pad and channels). Works on "
            "virtual pads, so a driven session verifies rumble without hardware. Takes no "
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

            const vector<Haptics::RumbleOneShotInfo> oneShots = engine->GetOneShots();
            const vector<Haptics::RumbleLayerInfo> layers = engine->GetLayers();

            Json pads = Json::array();
            for (u32 slot = 0; slot < Input::MaxGamepads; ++slot)
            {
                const auto pad = static_cast<GamepadId>(slot);
                const Haptics::RumbleChannels output = engine->GetOutput(pad);
                const auto shotCount = static_cast<usize>(
                    std::ranges::count_if(oneShots, [pad](const Haptics::RumbleOneShotInfo& info)
                                          { return info.Gamepad == pad; }));
                const auto layerCount = static_cast<usize>(
                    std::ranges::count_if(layers, [pad](const Haptics::RumbleLayerInfo& info)
                                          { return info.Gamepad == pad; }));
                if (shotCount == 0 && layerCount == 0 && output == Haptics::RumbleChannels{})
                {
                    continue;
                }
                pads.push_back(Json{{"pad", slot},
                                    {"one_shot_count", shotCount},
                                    {"layer_count", layerCount},
                                    {"output", ChannelsJson(output)}});
            }

            Json shots = Json::array();
            for (const Haptics::RumbleOneShotInfo& info : oneShots)
            {
                shots.push_back(Json{
                    {"scope", info.Scope.Value},
                    {"state", StateName(info.State)},
                    {"pad", PadOrNull(info.Gamepad)},
                    {"clip", fmt::format("0x{:016X}", info.Clip.Value)},
                    {"time", info.Time},
                    {"duration", info.Duration},
                    {"intensity", info.Intensity},
                });
            }

            Json mixed = Json::array();
            for (const Haptics::RumbleLayerInfo& info : layers)
            {
                mixed.push_back(Json{{"scope", info.Scope.Value},
                                     {"state", StateName(info.State)},
                                     {"pad", PadOrNull(info.Gamepad)},
                                     {"channels", ChannelsJson(info.Channels)}});
            }

            return Json{{"master_intensity", engine->GetMasterIntensity()},
                        {"suspended", engine->IsOutputSuspended()},
                        {"pads", std::move(pads)},
                        {"one_shots", std::move(shots)},
                        {"layers", std::move(mixed)}}
                .dump();
        };
        server.RegisterTool(std::move(tool));
    }
}
