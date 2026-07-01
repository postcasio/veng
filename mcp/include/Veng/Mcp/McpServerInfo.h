#pragma once

#include <Veng/Veng.h>

namespace Veng::Mcp
{
    /// @brief Construction descriptor for McpServer (the designated-init XInfo idiom).
    struct McpServerInfo
    {
        /// @brief Server name reported to the client in the `initialize` handshake.
        string ServerName = "veng";

        /// @brief TCP port to bind. 0 picks an ephemeral port readable via GetPort().
        u16 Port = 0;

        /// @brief When true, bind to 127.0.0.1 only; otherwise bind to all interfaces.
        bool BindLoopbackOnly = true;

        /// @brief When true, mutation tools are exposed. Off by default (a read-only surface).
        bool AllowMutations = false;
    };
}
