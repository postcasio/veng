#pragma once

// Export.h — shared-library symbol annotations.
//
// Every veng shared library builds with hidden symbol visibility on every platform, so a symbol
// crosses an image boundary only when its declaration says so. Windows enforces that through
// dllexport/dllimport; macOS and Linux enforce it through -fvisibility=hidden, so a missing
// annotation fails the link on any platform rather than only on the one that needs it.
//
// Each library owns a two-sided macro: it expands to VE_DLL_EXPORT while that library's own
// sources compile (they define its *_BUILD_SHARED symbol privately) and to VE_DLL_IMPORT
// everywhere else. A consumer library defines its own macro the same way over these two
// primitives rather than reusing VE_API, which always means "defined in libveng".
//
// What to annotate: a non-template class or struct with any member the library defines out of
// line (a non-inline member function, a non-inline static data member, a vtable keyed to an
// out-of-line virtual) takes the macro on the class; a free function or extern variable defined
// in the library takes it on the declaration. Header-only types, templates, and aggregates need
// none. On MSVC a class-level annotation does not reach nested classes, so a nested class with
// out-of-line members carries its own.

#if defined(_WIN32)
/// @brief Marks a declaration as defined in, and exported from, the image being built.
#define VE_DLL_EXPORT __declspec(dllexport)
/// @brief Marks a declaration as imported from another image.
#define VE_DLL_IMPORT __declspec(dllimport)
#else
/// @brief Marks a declaration as defined in, and exported from, the image being built.
#define VE_DLL_EXPORT __attribute__((visibility("default")))
/// @brief Marks a declaration as imported from another image.
#define VE_DLL_IMPORT __attribute__((visibility("default")))
#endif

/// @brief Export/import annotation for a symbol defined in libveng.
#if defined(VE_BUILD_SHARED)
#define VE_API VE_DLL_EXPORT
#else
#define VE_API VE_DLL_IMPORT
#endif

/// @brief Export annotation for a module's C-ABI entry point.
///
/// A module always defines and exports VengModuleRegister, so this is unconditionally the export
/// side. VE_API cannot be reused: in a module build it is the import side, which would fail the
/// link on the symbol the module itself defines.
#define VE_MODULE_EXPORT VE_DLL_EXPORT
