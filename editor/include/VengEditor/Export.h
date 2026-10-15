#pragma once

#include <Veng/Export.h>

/// @brief Export/import annotation for a symbol defined in libveng_editor (see Veng/Export.h).
#if defined(VE_EDITOR_BUILD_SHARED)
#define VE_EDITOR_API VE_DLL_EXPORT
#else
#define VE_EDITOR_API VE_DLL_IMPORT
#endif
