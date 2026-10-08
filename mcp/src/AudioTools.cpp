#include "AudioTools.h"
#include "PresentationStateName.h"

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
        // audio.list_voices — the device's live mix: every active voice's routing, pose and owning
        // scope, plus the music director's current track and the music requests it arbitrates.
        // Read-only, so it is always registered.
        McpTool tool;
        tool.Name = "audio.list_voices";
        tool.Description =
            "Lists every active audio voice on the device: each voice's bus, gain, pan, pitch, "
            "occlusion, reverb send, looping flag, whether it is a clip or a generator, its role "
            "(source/oneshot/spatial/music), its owning presentation scope and that scope's state "
            "(live, muted — mixed silent while it advances — or held — frozen), and — for a "
            "spatial voice — its world position and velocity. Also reports the music director's "
            "current track (a hex AssetId or null) and gain, the scope whose music request won "
            "(or null), and every scope's standing request — its track, priority, fade and loop, "
            "the scope's state and presentation rank (0 is the primary viewport; null when "
            "nothing presents it), and whether it is eligible to play — and the total active-voice "
            "count. Takes no arguments.";
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
                    {"bus", engine->GetBusName(info.Bus)},
                    {"origin", OriginName(info.Origin)},
                    {"source", info.Generator ? "generator" : "clip"},
                    {"gain", info.Gain},
                    {"pan", info.Pan},
                    {"pitch", info.Pitch},
                    {"occlusion", info.Occlusion},
                    {"reverb_send", info.ReverbSend},
                    {"loop", info.Loop},
                    {"spatial", info.Spatial},
                    {"scope", info.Scope.Value},
                    {"state", PresentationStateName(info.State)},
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

            const PresentationScopes& scopes = engine->GetScopes();
            Json requests = Json::array();
            for (const Audio::MusicRequestInfo& info : engine->GetMusicRequests())
            {
                const optional<u32> rank = scopes.GetPresentationRank(info.Scope);
                requests.push_back(Json{
                    {"scope", info.Scope.Value},
                    {"state", PresentationStateName(scopes.GetState(info.Scope))},
                    {"rank", rank.has_value() ? Json(*rank) : Json(nullptr)},
                    {"eligible", info.Eligible},
                    {"track", HexIdOrNull(info.Request.Track.Id())},
                    {"priority", info.Request.Priority},
                    {"fade_seconds", info.Request.FadeSeconds},
                    {"loop", info.Request.Loop},
                });
            }
            const PresentationScopeId winner = engine->GetMusicWinner();
            const Audio::MusicDirector& music = engine->Music();
            Json musicJson{
                {"track", HexIdOrNull(music.Current().Id())},
                {"gain", music.GetGain()},
                {"voice_count", music.GetVoiceCount()},
                {"winner_scope", winner.IsValid() ? Json(winner.Value) : Json(nullptr)},
                {"requests", std::move(requests)},
            };

            return Json{{"active_count", engine->GetActiveVoiceCount()},
                        {"voices", std::move(voices)},
                        {"music", std::move(musicJson)}}
                .dump();
        };
        server.RegisterTool(std::move(tool));
    }
}
