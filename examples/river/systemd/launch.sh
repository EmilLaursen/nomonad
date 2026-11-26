#!/bin/sh
export XDG_CURRENT_DESKTOP=river
export XDG_SESSION_TYPE=wayland
export XDG_SESSION_DESKTOP=river

if [ -d /sys/module/nvidia ]; then
    export __GLX_VENDOR_LIBRARY_NAME=nvidia
    export LIBVA_DRIVER_NAME=nvidia
    export WLR_RENDERER=vulkan
fi

here=$(dirname "$(readlink -f "$0")")
exec "$here/scope.sh" river river 2>"${XDG_RUNTIME_DIR:-/tmp}/river.log"
