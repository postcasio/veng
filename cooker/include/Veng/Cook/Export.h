#pragma once

#include <Veng/Export.h>

/// @brief Export/import annotation for a symbol defined in libveng_cook (see Veng/Export.h).
#if defined(VE_COOK_BUILD_SHARED)
#define VE_COOK_API VE_DLL_EXPORT
#else
#define VE_COOK_API VE_DLL_IMPORT
#endif
