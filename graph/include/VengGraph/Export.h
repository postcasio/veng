#pragma once

#include <Veng/Export.h>

/// @brief Export/import annotation for a symbol defined in libveng_graph (see Veng/Export.h).
#if defined(VE_GRAPH_BUILD_SHARED)
#define VE_GRAPH_API VE_DLL_EXPORT
#else
#define VE_GRAPH_API VE_DLL_IMPORT
#endif
