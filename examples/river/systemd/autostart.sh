#!/bin/sh
dbus-update-activation-environment --systemd WAYLAND_DISPLAY DISPLAY \
    XDG_SESSION_TYPE XDG_CURRENT_DESKTOP

pgrep -x mako >/dev/null || mako &
pgrep -x swayidle >/dev/null || swayidle -w before-sleep 'swaylock -f' &
pgrep -x waybar >/dev/null || waybar &
