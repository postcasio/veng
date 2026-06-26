# Cook the template asset pack with vengc at build time, into the build tree.
# veng_add_game copies the host-default config's cooked pack beside the launcher
# (renamed to the un-suffixed mount name); the app mounts it from
# ExecutableDirectory() so the launcher + module + pack are relocatable.
set(TEMPLATE_ASSET_DIR "${CMAKE_CURRENT_BINARY_DIR}/assets")

# The project owns its configuration set (configs/project.veng lists the three ship
# targets with macOS active) and the host -> config map: a name -> buildcfg lookup,
# since the engine owns only the selection variable and host-triple default.
# VENG_BUILD_CONFIG (defaulting from the host triple) names the configuration the
# launcher's pack cooks under.
set(TEMPLATE_CONFIG_macos   ${CMAKE_CURRENT_SOURCE_DIR}/configs/macos.buildcfg)
set(TEMPLATE_CONFIG_windows ${CMAKE_CURRENT_SOURCE_DIR}/configs/windows.buildcfg)
set(TEMPLATE_CONFIG_linux   ${CMAKE_CURRENT_SOURCE_DIR}/configs/linux.buildcfg)
set(TEMPLATE_SHIP_CONFIGS macos windows linux)

if (NOT DEFINED TEMPLATE_CONFIG_${VENG_BUILD_CONFIG})
    message(FATAL_ERROR
            "template: no build configuration '${VENG_BUILD_CONFIG}' (have: ${TEMPLATE_SHIP_CONFIGS})")
endif ()

# Cook every ship configuration into its own per-config pack target so cook-all-packs
# can build them all; the launcher copy below wires only the host-default pack.
foreach (CFG IN LISTS TEMPLATE_SHIP_CONFIGS)
    add_asset_pack(template_assets
            PACK   assets/template.vengpack.json
            OUTPUT ${TEMPLATE_ASSET_DIR}/template.vengpack
            CONFIG ${TEMPLATE_CONFIG_${CFG}}
            # The material's vertex shader references the core canonical vertex layout
            # by AssetId; resolve it from the engine core pack.
            REFERENCE ${CMAKE_SOURCE_DIR}/engine/assets/core/core.vengpack.json)
    veng_register_all_packs_target(${template_assets_TARGET})
    if (CFG STREQUAL VENG_BUILD_CONFIG)
        set(TEMPLATE_HOST_PACK_TARGET ${template_assets_TARGET})
    endif ()
endforeach ()

# The game splits into libtemplate (the runtime) and template-launcher (the shipped exe
# that dlopens it). veng_add_game copies the host-default config's cooked pack beside the
# launcher so the trio is a relocatable directory.
veng_add_game(template
        SOURCES    main.cpp
        ASSET_PACK ${TEMPLATE_HOST_PACK_TARGET})
