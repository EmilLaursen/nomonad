
                                                            _
          _ __    ___   _ __ ___    ___   _ __    __ _   __| |
         | '_ \  / _ \ | '_ ` _ \  / _ \ | '_ \  / _` | / _` |
         | | | || (_) || | | | | || (_) || | | || (_| || (_| |
         |_| |_| \___/ |_| |_| |_| \___/ |_| |_| \__,_| \__,_|

         an homage to xmonad, but it is no monad.


Nomonad exists because I am a very happy Xmonad user, but wanted to
switch to wayland. River has trivialized writing your own WM, so this is
a subset of the xmonad features I care about, with sufficiently similar
behavior.

Each output shows one of nine workspaces, a workspace has a stack of
windows and a cycle of layouts, there is a master area, floating
windows, named and dynamic scratchpads, submaps and per-window rules.
The configuration is a small Lisp:

    (defcfg
      (borders :width 2 :focused "#21fc0d" :unfocused "#4c566a")
      (gaps :inner 4 :smart true)
      (layouts tall bsp deck)
      (keymap :layout "us,dk" :options "grp:alt_shift_toggle")
      (repeat :rate 50 :delay 300))

    (defkeys
      (M Return  (spawn "foot"))
      (M q       close)
      (M j       focus-next)
      (M k       focus-prev)
      (M space   next-layout)
      (M 1 (workspace 1)) (M-S 1 (move-to-workspace 1)))

    (defrules
      (float (app-id "pavucontrol") :center true))

The file is watched and reloaded on save, and the running binary can be
recompiled and hotswapped.

========================================================================
BUILDING AND INSTALLING
========================================================================

To run it you need river 0.5. The river protocol files are vendored in
protocol/, nomonad does not link against river.

On Arch:

    $ sudo pacman -S base-devel wayland libxkbcommon river

Then:

    $ make
    $ make test
    $ sudo make install

The build goes to build/, or use make O=DIR.

make install puts the binary in /usr/local/bin and the README, LICENSE,
examples and config reference in /usr/local/share/doc/nomonad. PREFIX
and DESTDIR work as usual:

    $ make install PREFIX=$HOME/.local

make install-contrib adds the helper scripts from contrib/.

========================================================================
STARTING A SESSION
========================================================================

At login something starts river, river runs ~/.config/river/init, and
the init starts nomonad. The init is a shell script and must be
executable. A small example is examples/river/minimal/init:

    #!/bin/sh
    waybar &
    mako &
    nomonad &

examples/river/systemd/ is a fuller setup:

    launch.sh       what the greeter runs: exports the session
                    variables (and NVIDIA ones when that driver is
                    loaded), then starts river
    scope.sh        runs a command in its own systemd scope
    init            river's init
    autostart.sh    the activation environment and helpers

Copy the four files to ~/.config/river/ and point the greeter at
launch.sh, for example --cmd ~/.config/river/launch.sh with tuigreet.

nomonad starts programs from spawn through a small helper process,
each in its own systemd scope when a user manager is available. A
program that crashes or is killed takes only its own scope with it.


========================================================================
CONFIGURATION
========================================================================

nomonad reads $XDG_CONFIG_HOME/nomonad/config.scm, by default
~/.config/nomonad/config.scm, or the file given with --config.

Without a config, or when the config has an error at startup, nomonad
runs a built-in default with xmonad's default keys:

    Super+Shift+Return    terminal, $TERMINAL or foot
    Super+p               launcher, fuzzel
    Super+Shift+c         close the focused window
    Super+space           next layout: tall, mirror-tall, deck
    Super+j / k / Tab     focus next / previous / next
    Super+m               focus the master window
    Super+Shift+j / k     swap with the next / previous window
    Super+h / l           shrink / grow the master area
    Super+comma / period  more / fewer windows in the master area
    Super+t               tile or float the focused window
    Super+1..9            show workspace N
    Super+Shift+1..9      send the focused window to workspace N
    Super+w / e / r       focus output 1 / 2 / 3
    Super+Shift+w / e / r send the focused window there
    Super+q               restart nomonad in place
    Super+Shift+q         end the session
    Super+left drag       move a window
    Super+right drag      resize a window

examples/config.scm is the place to start, and docs/config.md. It is
generated from the binary:

    $ nomonad --describe

prints the whole config language as s-expressions, and make docs
turns that into docs/config.md.

Keyboards, pointers and touchpads are configured here too, applied to
every matching device including ones plugged in later:

    (defcfg
      (keymap :layout "us,dk" :options "grp:alt_shift_toggle")
      (repeat :rate 50 :delay 300)
      (cursor :theme "Adwaita" :size 24)
      (touchpad :tap true :natural-scroll true :dwt true))

A keymap takes the xkb names :rules :model :layout :variant and
:options. The cursor block exports XCURSOR_THEME and XCURSOR_SIZE to
programs nomonad starts.

The config is reloaded when the file is saved, on SIGUSR1, or by the
reload-config action. The restart action, or SIGUSR2, makes nomonad save
its state to $XDG_RUNTIME_DIR, exec the binary from disk and restore:
window placement, floating geometry, fullscreen, scratchpads, layouts
including bsp trees, visible workspaces and focus. Windows stay open and
river keeps running. Therefore, the binary can be updated while running.

========================================================================
TALKING TO A RUNNING NOMONAD
========================================================================

nomonad listens on $XDG_RUNTIME_DIR/nomonad-ctl.sock. One command per
line, answered by ok or error MESSAGE. nomonad -m sends one and prints
the answer:

    $ nomonad -m list               id, workspace, focused, pid,
                                    app_id and title, one window a line
    $ nomonad -m 'focus-window ID'  go to its workspace and output,
                                    focus it
    $ nomonad -m 'close-window ID'
    $ nomonad -m '(workspace 3)'    any config action, except pointer
                                    operations and submaps

contrib/ has three small programs built on this:

    nomonad-switch  pick a window with rofi and jump to it
    nomonad-fzf     the same with fzf, previewing each window with
                    grim, inside kitty
    nomonad-keys    every key binding of the running config, with the
                    documentation of its action, in rofi

They find nomonad on PATH, or through $NOMONAD.

nomonad also publishes its state, outputs, workspaces, layouts and
windows, as logfmt on $XDG_RUNTIME_DIR/nomonad-status.sock whenever
it changes. That is meant for status bars and scripts, see
docs/status-stream.md.

Other options:

    nomonad --config PATH   use another config file
    nomonad --describe      the config schema
    nomonad --version
    nomonad --help


========================================================================
LAYOUTS AND BUILD OPTIONS
========================================================================

The layouts are tall, mirror-tall, resizable-tall, three-columns,
two-pane, grid, spiral, deck, tabbed and bsp. Every workspace cycles
through the ones listed in (layouts ...), next-layout and prev-layout
walk the cycle. The tall family has a master area that grows, shrinks
and takes more or fewer windows. bsp keeps a binary tree per
workspace, with actions to rotate, swap, balance and resize its
splits.

EXTRA_CFLAGS adds to the compiler flags. Two knobs are meant to be set
this way:

    -DNM_LAYOUTS_MAX=N
        the most layouts one workspace can cycle through, 16 by
        default. Each workspace holds that many layout slots of about
        1.3 KB each.

    -DNM_NO_<KIND>
        leaves a layout out of the binary, and its name out of the
        config language. KIND is one of BSP, DECK, MIRROR_TALL,
        THREE_COLUMNS, GRID, TABBED, SPIRAL, TWO_PANE and
        RESIZABLE_TALL. tall is always there.

    $ make EXTRA_CFLAGS="-DNM_NO_DECK -DNM_NO_GRID -DNM_NO_SPIRAL"

A full build is about 145 KB.


========================================================================
DEVELOPMENT
========================================================================

    $ make test         unit tests for the reader and the config
                        parser, and the docs/config.md check
    $ make harness      the headless harness: the whole window manager
                        driven in process against a fake Wayland, with
                        invariants checked after every cycle
    $ make fuzz         a libFuzzer driver over the same harness
    $ ./run-nested.sh   nomonad in a river running in a window

See docs/development.md and docs/session-format.md.
