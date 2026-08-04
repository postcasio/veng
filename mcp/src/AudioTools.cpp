#include "AudioTools.h"

#include <Veng/Mcp/McpHost.h>
#include <Veng/Mcp/McpServer.h>
#include <Veng/Mcp/McpTool.h>

#include <Veng/Audio/AudioEngine.h>

#include <nlohmann/json.hpp>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief The bus enumerator name, matching the reflected AudioBus spelling.
        const char* BusName(const Audio::AudioBus bus)
        {
            switch (bus)
            {
            case Audio::AudioBus::Master:
                return "Master";
            case Audio::AudioBus::Music:
                return "Music";
            case Audio::AudioBus::SFX:
                return "SFX";
            case Audio::AudioBus::UI:
                return "UI";
            case Audio::AudioBus::Ambience:
                return "Ambience";
            }
            return "Unknown";
        }

        /// @brief The voice-origin name reported for a voice.
        const char* OriginName(const Audio::VoiceOrigin origin)
        {
            switch (origin)
            {
            case Audio::VoiceOrigin::Source:
                return "source";
            case Audio::VoiceOrigin::OneShot:
                return "oneshot";
            case Audio::VoiceOrigin::Spatial:
                return "spatial";
            case Audio::VoiceOrigin::Music:
                return "music";
            }
            return "unknown";
        }

        /// @brief A canonical hex AssetId string, or null for an invalid id.
        Json HexIdOrNull(const AssetId id)
        {
            if (!id.IsValid())
            {
                return nullptr;
            }
            return fmt::format("0x{:016X}", id.Value);
        }
    }

    void RegisterAudioTools(McpServer& server, const McpHost& host)
    {
        // audio.list_voices — the live mix of the presented world: every active voice's routing and
        // pose, plus the music director's current track. Read-only, so it is always registered.
        McpTool tool;
        tool.Name = "audio.list_voices";
        tool.Description =
            "Lists the active audio voices in the presented world's mix: each voice's bus, gain, "
            "pan, pitch, occlusion, reverb send, looping flag, whether it is a clip or a "
            "generator, "
            "its role (source/oneshot/spatial/music), and — for a spatial voice — its world "
            "position and velocity. Also reports the music director's current track (a hex AssetId "
            "or null) and gain, and the total active-voice count. Takes no arguments.";
        tool.InputSchemaJson = R"({"type":"object","properties":{}})";
        tool.Handler = [&host](string_view) -> Result<string>
        {
            Audio::AudioEngine* const engine = host.Audio ? host.Audio() : nullptr;
            if (engine == nullptr)
            {
                return std::unexpected(
                    string("audio is unavailable: this host exposes no audio engine"));
            }

            Json voices = Json::array();
            for (const Audio::VoiceInfo& info : engine->GetVoiceInfos())
            {
                Json item{
                    {"slot", info.Handle.Slot},
                    {"generation", info.Handle.Generation},
                    {"bus", BusName(info.Bus)},
                    {"origin", OriginName(info.Origin)},
                    {"source", info.Generator ? "generator" : "clip"},
                    {"gain", info.Gain},
                    {"pan", info.Pan},
                    {"pitch", info.Pitch},
                    {"occlusion", info.Occlusion},
                    {"reverb_send", info.ReverbSend},
                    {"loop", info.Loop},
                    {"spatial", info.Spatial},
                };
                if (info.Spatial)
                {
                    item["position"] =
                        Json::array({info.Position.x, info.Position.y, info.Position.z});
                    item["velocity"] =
                        Json::array({info.Velocity.x, info.Velocity.y, info.Velocity.z});
                }
                voices.push_back(std::move(item));
            }

            const Audio::MusicDirector& music = engine->Music();
            Json musicJson{
                {"track", HexIdOrNull(music.Current().Id())},
                {"gain", music.GetGain()},
                {"voice_count", music.GetVoiceCount()},
            };

            return Json{{"active_count", engine->GetActiveVoiceCount()},
                        {"voices", std::move(voices)},
                        {"music", std::move(musicJson)}}
                .dump();
        };
        server.RegisterTool(std::move(tool));
    }
}
