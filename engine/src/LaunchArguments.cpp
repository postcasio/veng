#include <Veng/LaunchArguments.h>

#include <algorithm>
#include <charconv>

#include <fmt/format.h>

namespace Veng
{
    namespace
    {
        // Parses an AssetId operand: decimal, or hex with a 0x/0X prefix. The reserved 0 id is
        // rejected so `--level=0` fails loudly rather than silently selecting the invalid id.
        Result<AssetId> ParseAssetId(const string_view text)
        {
            string_view digits = text;
            int base = 10;
            if (digits.starts_with("0x") || digits.starts_with("0X"))
            {
                digits.remove_prefix(2);
                base = 16;
            }

            u64 value = 0;
            const char* const begin = digits.data();
            const char* const end = begin + digits.size();
            const std::from_chars_result parsed = std::from_chars(begin, end, value, base);
            if (parsed.ec != std::errc{} || parsed.ptr != end || digits.empty())
            {
                return std::unexpected(fmt::format("invalid level id '{}'", text));
            }
            if (value == 0)
            {
                return std::unexpected(fmt::format("invalid level id '{}': 0 is reserved", text));
            }
            return AssetId{value};
        }

        // Parses a `--join` operand `host[:port]` into a JoinTarget. The host is required; a trailing
        // `:port` overrides the app's default port. Splits on the last ':' so a bare host with no port
        // resolves the whole operand as the host. IPv6 literals are out of scope for the v1 target.
        Result<JoinTarget> ParseJoinTarget(const string_view text)
        {
            if (text.empty())
            {
                return std::unexpected(string("--join requires a host"));
            }

            const usize colon = text.find_last_of(':');
            if (colon == string_view::npos)
            {
                return JoinTarget{.Host = string(text), .Port = 0};
            }

            const string_view host = text.substr(0, colon);
            const string_view portText = text.substr(colon + 1);
            if (host.empty())
            {
                return std::unexpected(fmt::format("invalid join target '{}': empty host", text));
            }

            u16 port = 0;
            const char* const begin = portText.data();
            const char* const end = begin + portText.size();
            const std::from_chars_result parsed = std::from_chars(begin, end, port);
            if (parsed.ec != std::errc{} || parsed.ptr != end || portText.empty())
            {
                return std::unexpected(fmt::format("invalid join target '{}': bad port", text));
            }
            return JoinTarget{.Host = string(host), .Port = port};
        }

        // Parses a `--netsim` operand `key=value,key=value` into a fault/latency config. Keys:
        // latency/jitter (milliseconds), loss/dup/reorder (percentages → [0,1] rates), seed (u64).
        Result<Net::FaultInjectionConfig> ParseNetSim(const string_view text)
        {
            Net::FaultInjectionConfig config;
            usize start = 0;
            while (start <= text.size())
            {
                const usize comma = text.find(',', start);
                const string_view pair = text.substr(
                    start, comma == string_view::npos ? string_view::npos : comma - start);
                start = comma == string_view::npos ? text.size() + 1 : comma + 1;
                if (pair.empty())
                {
                    continue;
                }
                const usize eq = pair.find('=');
                if (eq == string_view::npos)
                {
                    return std::unexpected(fmt::format("invalid --netsim term '{}'", pair));
                }
                const string_view key = pair.substr(0, eq);
                const string_view valueText = pair.substr(eq + 1);
                f32 value = 0.0f;
                const char* const begin = valueText.data();
                const char* const end = begin + valueText.size();
                const std::from_chars_result parsed = std::from_chars(begin, end, value);
                if (parsed.ec != std::errc{} || parsed.ptr != end || valueText.empty())
                {
                    return std::unexpected(fmt::format("invalid --netsim value '{}'", pair));
                }

                if (key == "latency")
                {
                    config.LatencyMs = value;
                }
                else if (key == "jitter")
                {
                    config.JitterMs = value;
                }
                else if (key == "loss")
                {
                    config.DropRate = value / 100.0f;
                }
                else if (key == "dup")
                {
                    config.DuplicateRate = value / 100.0f;
                }
                else if (key == "reorder")
                {
                    config.ReorderRate = value / 100.0f;
                }
                else if (key == "seed")
                {
                    config.Seed = static_cast<u64>(value);
                }
                else
                {
                    return std::unexpected(fmt::format("unknown --netsim key '{}'", key));
                }
            }
            return config;
        }
    }

    Result<LaunchArguments> LaunchArguments::Parse(const std::span<const string> args,
                                                   const std::span<const LaunchOptionInfo> options)
    {
        LaunchArguments result;

        for (usize i = 0; i < args.size(); ++i)
        {
            const string_view arg = args[i];

            if (arg == "--level" || arg.starts_with("--level="))
            {
                string_view value;
                if (arg.starts_with("--level="))
                {
                    value = arg.substr(std::string_view("--level=").size());
                }
                else if (i + 1 < args.size())
                {
                    value = args[++i];
                }
                else
                {
                    return std::unexpected(string("--level requires a value"));
                }

                const Result<AssetId> id = ParseAssetId(value);
                if (!id)
                {
                    return std::unexpected(id.error());
                }
                result.Level = *id;
            }
            else if (arg == "--server")
            {
                result.Server = true;
            }
            else if (arg == "--headless")
            {
                result.Headless = true;
            }
            else if (arg == "--dedicated")
            {
                // The first-class dedicated-server flag: a headless listen host with no window, no
                // managed viewport, and no local seat — the honest name for `--server --headless`, so
                // it sets both arms and drives the identical ServerHost path.
                result.Dedicated = true;
                result.Server = true;
                result.Headless = true;
            }
            else if (arg == "--join" || arg.starts_with("--join="))
            {
                string_view value;
                if (arg.starts_with("--join="))
                {
                    value = arg.substr(std::string_view("--join=").size());
                }
                else if (i + 1 < args.size())
                {
                    value = args[++i];
                }
                else
                {
                    return std::unexpected(string("--join requires a value"));
                }

                const Result<JoinTarget> target = ParseJoinTarget(value);
                if (!target)
                {
                    return std::unexpected(target.error());
                }
                result.Join = *target;
            }
            else if (arg == "--name" || arg.starts_with("--name="))
            {
                string_view value;
                if (arg.starts_with("--name="))
                {
                    value = arg.substr(std::string_view("--name=").size());
                }
                else if (i + 1 < args.size())
                {
                    value = args[++i];
                }
                else
                {
                    return std::unexpected(string("--name requires a value"));
                }
                if (value.empty())
                {
                    return std::unexpected(string("--name requires a non-empty value"));
                }
                result.Name = string(value);
            }
            else if (arg == "--netsim" || arg.starts_with("--netsim="))
            {
                string_view value;
                if (arg.starts_with("--netsim="))
                {
                    value = arg.substr(std::string_view("--netsim=").size());
                }
                else if (i + 1 < args.size())
                {
                    value = args[++i];
                }
                else
                {
                    return std::unexpected(string("--netsim requires a value"));
                }

                const Result<Net::FaultInjectionConfig> netsim = ParseNetSim(value);
                if (!netsim)
                {
                    return std::unexpected(netsim.error());
                }
                result.NetSim = *netsim;
            }
            else if (arg.starts_with("--"))
            {
                // An application-declared option widens the known set. The engine's own flags are
                // matched above, so a name shared with one resolves to the engine flag.
                const string_view body = arg.substr(2);
                const usize equals = body.find('=');
                const string_view name = body.substr(0, equals);
                const auto declared =
                    std::ranges::find_if(options, [name](const LaunchOptionInfo& option)
                                         { return option.Name == name; });
                if (declared == options.end())
                {
                    return std::unexpected(fmt::format("unknown argument '{}'", arg));
                }

                if (!declared->TakesValue)
                {
                    if (equals != string_view::npos)
                    {
                        return std::unexpected(fmt::format("--{} takes no value", name));
                    }
                    result.GameOptions[string(name)] = string();
                }
                else
                {
                    string_view value;
                    if (equals != string_view::npos)
                    {
                        value = body.substr(equals + 1);
                    }
                    else if (i + 1 < args.size())
                    {
                        value = args[++i];
                    }
                    else
                    {
                        return std::unexpected(fmt::format("--{} requires a value", name));
                    }
                    result.GameOptions[string(name)] = string(value);
                }
            }
            else if (!result.WorkingDirectory)
            {
                result.WorkingDirectory = path(arg);
            }
            else
            {
                return std::unexpected(fmt::format("unexpected argument '{}'", arg));
            }
        }

        return result;
    }
}
