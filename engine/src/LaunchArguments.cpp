#include <Veng/LaunchArguments.h>

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
    }

    Result<LaunchArguments> LaunchArguments::Parse(const std::span<const string> args)
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
            else if (arg.starts_with("--"))
            {
                return std::unexpected(fmt::format("unknown argument '{}'", arg));
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
