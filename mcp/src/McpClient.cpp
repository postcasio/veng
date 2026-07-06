#include <Veng/Mcp/McpClient.h>

#include <nlohmann/json.hpp>

// httplib appears here only, never in a public header — it is a PRIVATE dependency
// hidden behind the Native idiom. This TU is the CALLER of httplib and nlohmann, the
// opposite position from McpServer.cpp: a throw from either would propagate OUT of this
// TU into the -fno-exceptions front-end, which is UB. So the containment is explicit and
// load-bearing here — every nlohmann read is non-throwing, every dump uses the replace
// handler, and each public method's whole body is wrapped in a TU-local try/catch that
// converts any escape into a Result error. Compiled -fexceptions (see mcp/CMakeLists.txt).
#include "Vendor/httplib.h"

#include <fmt/format.h>

namespace Veng::Mcp
{
    using Json = nlohmann::json;

    namespace
    {
        /// @brief Serializes JSON with the non-throwing UTF-8 handler.
        ///
        /// dump() throws type_error 316 on invalid UTF-8 (a stray byte in a tool
        /// description or inputSchema); the replace handler substitutes it instead of
        /// throwing, matching this TU's no-escape contract.
        string DumpSafe(const Json& value)
        {
            return value.dump(-1, ' ', false, Json::error_handler_t::replace);
        }

        /// @brief Extracts a JSON-RPC `error` object's message into a located string.
        string RpcErrorText(const Json& error)
        {
            const string message = error.value("message", string{});
            const int code = error.value("code", 0);
            if (!message.empty())
            {
                return fmt::format("rpc error {}: {}", code, message);
            }
            return fmt::format("rpc error {}", code);
        }
    }

    /// @brief Backend state: the vendored httplib client, hidden behind the Native idiom.
    struct McpClient::Native
    {
        /// @brief The loopback HTTP client; connects lazily on the first POST.
        httplib::Client Http;

        Native(const string& host, u16 port) : Http(host, port) {}
    };

    McpClient::McpClient() = default;

    McpClient::~McpClient() = default;

    Result<Unique<McpClient>> McpClient::Create(const McpClientInfo& info)
    {
        // The httplib::Client construction and its socket/string setup can throw
        // std::bad_alloc, so wrap the whole body — nothing may cross into the caller.
        try
        {
            Unique<McpClient> client{new McpClient()};
            client->m_Native = std::make_unique<Native>(info.Host, info.Port);

            const auto timeout = static_cast<time_t>(info.TimeoutSeconds);
            client->m_Native->Http.set_connection_timeout(timeout, 0);
            client->m_Native->Http.set_read_timeout(timeout, 0);
            client->m_Native->Http.set_write_timeout(timeout, 0);

            return client;
        }
        catch (...)
        {
            return std::unexpected(
                fmt::format("failed to create MCP client for {}:{}", info.Host, info.Port));
        }
    }

    Result<vector<McpToolDesc>> McpClient::ListTools()
    {
        try
        {
            const Json request = Json{{"jsonrpc", "2.0"},
                                      {"id", 1},
                                      {"method", "tools/list"},
                                      {"params", Json::object()}};

            const httplib::Result response =
                m_Native->Http.Post("/", DumpSafe(request), "application/json");
            if (!response)
            {
                return std::unexpected(string("no response from server"));
            }

            const Json body = Json::parse(response->body, nullptr, false);
            if (!body.is_object())
            {
                return std::unexpected(string("server reply was not a JSON object"));
            }

            const auto errorIt = body.find("error");
            if (errorIt != body.end() && errorIt->is_object())
            {
                return std::unexpected(RpcErrorText(*errorIt));
            }

            const auto resultIt = body.find("result");
            if (resultIt == body.end() || !resultIt->is_object())
            {
                return std::unexpected(string("server reply carried no result"));
            }

            const auto toolsIt = resultIt->find("tools");
            vector<McpToolDesc> tools;
            if (toolsIt != resultIt->end() && toolsIt->is_array())
            {
                for (const Json& tool : *toolsIt)
                {
                    if (!tool.is_object())
                    {
                        continue;
                    }
                    McpToolDesc desc;
                    desc.Name = tool.value("name", string{});
                    desc.Description = tool.value("description", string{});
                    const auto schemaIt = tool.find("inputSchema");
                    if (schemaIt != tool.end())
                    {
                        desc.InputSchema = DumpSafe(*schemaIt);
                    }
                    tools.emplace_back(std::move(desc));
                }
            }
            return tools;
        }
        catch (...)
        {
            return std::unexpected(string("unexpected failure listing tools"));
        }
    }

    Result<McpCallResult> McpClient::CallTool(string_view name, string_view argumentsJson)
    {
        try
        {
            // Parse the caller's arguments JSON into the request's `arguments` object; an
            // empty or unparseable string becomes an empty object.
            Json arguments = Json::parse(argumentsJson, nullptr, false);
            if (!arguments.is_object())
            {
                arguments = Json::object();
            }

            const Json request =
                Json{{"jsonrpc", "2.0"},
                     {"id", 1},
                     {"method", "tools/call"},
                     {"params", {{"name", string(name)}, {"arguments", arguments}}}};

            const httplib::Result response =
                m_Native->Http.Post("/", DumpSafe(request), "application/json");
            if (!response)
            {
                return std::unexpected(string("no response from server"));
            }

            const Json body = Json::parse(response->body, nullptr, false);
            if (!body.is_object())
            {
                return std::unexpected(string("server reply was not a JSON object"));
            }

            const auto errorIt = body.find("error");
            if (errorIt != body.end() && errorIt->is_object())
            {
                return std::unexpected(RpcErrorText(*errorIt));
            }

            const auto resultIt = body.find("result");
            if (resultIt == body.end() || !resultIt->is_object())
            {
                return std::unexpected(string("server reply carried no result"));
            }

            McpCallResult result;
            result.IsError = resultIt->value("isError", false);

            const auto contentIt = resultIt->find("content");
            if (contentIt != resultIt->end() && contentIt->is_array())
            {
                for (const Json& block : *contentIt)
                {
                    if (!block.is_object())
                    {
                        continue;
                    }
                    const string type = block.value("type", string{});
                    if (type == "text")
                    {
                        result.Content += block.value("text", string{});
                    }
                    else if (type == "image" && !result.Image.has_value())
                    {
                        result.Image = McpImageBlock{.MimeType = block.value("mimeType", string{}),
                                                     .Base64Data = block.value("data", string{})};
                    }
                }
            }
            return result;
        }
        catch (...)
        {
            return std::unexpected(string("unexpected failure calling tool"));
        }
    }
}
