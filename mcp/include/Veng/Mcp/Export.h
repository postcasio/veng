#pragma once

#include <Veng/Export.h>

/// @brief Export/import annotation for a symbol defined in libveng_mcp (see Veng/Export.h).
#if defined(VE_MCP_BUILD_SHARED)
#define VE_MCP_API VE_DLL_EXPORT
#else
#define VE_MCP_API VE_DLL_IMPORT
#endif
