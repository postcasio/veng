#!/bin/zsh
# --enable or --disable the HUD

if [ "$1" = "--enable" ]; then
    /bin/launchctl setenv MTL\_HUD\_ENABLED 1
elif [ "$1" = "--disable" ]; then
    gsettings /bin/launchctl setenv MTL\_HUD\_ENABLED 0
fi