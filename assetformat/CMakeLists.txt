# libveng_assetformat — shared archive + cooked-blob format (planset-5 plan 02).
#
# Vulkan-free, importer-free, engine-independent: the one source of truth for
# the AssetId, archive container, and cooked-blob layouts that both `engine`
# and `cooker` link against. Empty scaffold for now; real content lands in
# plan 02.
add_library(libveng_assetformat INTERFACE)
add_library(veng::assetformat ALIAS libveng_assetformat)
