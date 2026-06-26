# Cook the template asset pack with vengc at build time, into the build tree.
# veng_add_game copies the cooked pack beside the launcher; the app mounts it from
# ExecutableDirectory() so the launcher + module + pack are relocatable. The pack carries
# no project.veng, so it cooks via the zero-config codec default — the template stays
# minimal and decoupled from the build-configuration surface.
set(TEMPLATE_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")

add_asset_pack(template_assets
        PACK   assets/template.vengpack.json
        OUTPUT ${TEMPLATE_ASSET_DIR}/template.vengpack
        # The material's vertex shader references the core canonical vertex layout by
        # AssetId; resolve it from the engine core pack.
        REFERENCE ${CMAKE_SOURCE_DIR}/engine/assets/core/core.vengpack.json)

# The game splits into libtemplate (the runtime) and template-launcher (the shipped exe
# that dlopens it). veng_add_game copies the cooked pack beside the launcher so the trio
# is a relocatable directory.
veng_add_game(template
        SOURCES    main.cpp
        ASSET_PACK template_assets)
