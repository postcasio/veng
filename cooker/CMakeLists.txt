# libveng_cook + the vengc CLI (planset-5 plan 03).
#
# Offline importer/cooker: stb, assimp, Slang, and JSON pack parsing live here,
# never in the engine. Empty scaffold for now; real content lands in plan 03.
add_library(libveng_cook INTERFACE)
add_library(veng::cook ALIAS libveng_cook)
