#pragma once

#include <Veng/Veng.h>

namespace Veng::Mcp
{
    /// @brief Construction descriptor for McpClient (the designated-init XInfo idiom).
    struct McpClientInfo
    {
        /// @brief Server host. Loopback by default; set for a non-loopback server.
        string Host = "127.0.0.1";

        /// @brief The server's bound port.
        u16 Port = 0;

        /// @brief Connect + read timeout, in seconds. A stalled pump must not hang the caller.
        u32 TimeoutSeconds = 10;
    };
}
