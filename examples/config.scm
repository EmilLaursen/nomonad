;; nomonad example config
;;
;; Copy to ~/.config/nomonad/config.scm. nomonad reloads it on save.
;; docs/config.md is the full reference, nomonad --describe prints it.
;;
;; Modifiers: M super, A alt, C ctrl, S shift, combined as M-S or M-A-S.
;; () is no modifier, for media keys. Keys are xkb keysym names:
;; Return, space, Tab, grave, comma, F1, XF86AudioMute, ...

;; ---------------------------------------------------------------------
;; Settings

(defcfg
  ;; The layouts every workspace cycles through, the first one is used
  ;; at start. Also: mirror-tall, resizable-tall, two-pane, grid,
  ;; spiral, tabbed.
  (layouts tall bsp three-columns deck)

  ;; ssd asks clients to leave out their titlebars.
  (decorations ssd)

  ;; :smart drops borders and gaps on a workspace with a single tile.
  ;; :captured marks windows that are being screen shared.
  (borders :width 2 :focused "#21fc0d" :unfocused "#4c566a"
           :captured "#ff0000" :smart true)
  (gaps :inner 4 :smart true)

  ;; Input, applied to every matching device, also ones plugged in
  ;; later. Anything left out keeps river's default.
  (keymap :layout "us" :options "caps:escape")
  (repeat :rate 40 :delay 300)
  (cursor :theme "Adwaita" :size 24)
  (touchpad :tap true :natural-scroll true :dwt true))

;; @term and @menu below are replaced by these.
(defalias term (spawn "foot"))
(defalias menu (spawn "fuzzel"))

;; ---------------------------------------------------------------------
;; Keys

(defkeys
  ;; programs
  (M Return    @term)
  (M d         @menu)

  ;; windows
  (M q         close)
  (M f         toggle-fullscreen)
  (M t         toggle-float)
  (M-S s       toggle-sticky)

  ;; focus and order
  (M j         focus-next)
  (M k         focus-prev)
  (M m         focus-master)
  (M o         focus-last)
  (M-S j       swap-next)
  (M-S k       swap-prev)

  ;; Alt-tab style: most recently used first, the choice is final
  ;; when Super is released.
  (M Tab       focus-mru-next)
  (M-S Tab     focus-mru-prev)

  ;; layouts
  (M space     next-layout)
  (M-S space   prev-layout)

  ;; the master area of tall, mirror-tall, three-columns and friends
  (M h         shrink-master)
  (M l         expand-master)
  (M comma     inc-master)
  (M period    dec-master)

  ;; bsp
  (M r         bsp-rotate)
  (M s         bsp-swap)
  (M a         bsp-balance-area)
  (M-S a       bsp-equalize)
  (M-A h       bsp-expand-l)
  (M-A j       bsp-expand-d)
  (M-A k       bsp-expand-u)
  (M-A l       bsp-expand-r)
  (M-A-S h     bsp-shrink-l)
  (M-A-S j     bsp-shrink-d)
  (M-A-S k     bsp-shrink-u)
  (M-A-S l     bsp-shrink-r)
  (M-A Right   bsp-rotate-cw)
  (M-A Left    bsp-rotate-ccw)

  ;; resizable-tall: trade height with the neighbour in the column
  (M-A Up      rt-shrink-cell)
  (M-A Down    rt-expand-cell)

  ;; floating windows, by keyboard
  (M-C Left    (resize right -40))
  (M-C Right   (resize right +40))
  (M-C Up      (resize down  -40))
  (M-C Down    (resize down  +40))

  ;; Scratchpads. grave stashes the focused window, or brings it back.
  ;; The named ones are defined under defscratchpads below.
  (M grave     toggle-scratchpad)
  (M-S Return  (scratchpad term))
  (M c         (scratchpad calc))

  ;; Workspaces. Shift moves the window and follows it, Ctrl only
  ;; sends it.
  (M 1 (workspace 1)) (M-S 1 (move-to-workspace-follow 1)) (M-C 1 (move-to-workspace 1))
  (M 2 (workspace 2)) (M-S 2 (move-to-workspace-follow 2)) (M-C 2 (move-to-workspace 2))
  (M 3 (workspace 3)) (M-S 3 (move-to-workspace-follow 3)) (M-C 3 (move-to-workspace 3))
  (M 4 (workspace 4)) (M-S 4 (move-to-workspace-follow 4)) (M-C 4 (move-to-workspace 4))
  (M 5 (workspace 5)) (M-S 5 (move-to-workspace-follow 5)) (M-C 5 (move-to-workspace 5))
  (M 6 (workspace 6)) (M-S 6 (move-to-workspace-follow 6)) (M-C 6 (move-to-workspace 6))
  (M 7 (workspace 7)) (M-S 7 (move-to-workspace-follow 7)) (M-C 7 (move-to-workspace 7))
  (M 8 (workspace 8)) (M-S 8 (move-to-workspace-follow 8)) (M-C 8 (move-to-workspace 8))
  (M 9 (workspace 9)) (M-S 9 (move-to-workspace-follow 9)) (M-C 9 (move-to-workspace 9))
  (M bracketright next-workspace)
  (M bracketleft  prev-workspace)

  ;; outputs, counted from 0
  (M w         (focus-output 0))
  (M e         (focus-output 1))
  (M-S w       (move-to-output 0))
  (M-S e       (move-to-output 1))

  ;; session: Super+x, then one key, see defsubmap below
  (M x         (submap session))
  (M-S r       reload-config)
  (M-S q       exit)

  ;; media keys, no modifier
  (() XF86AudioRaiseVolume  (spawn "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%+"))
  (() XF86AudioLowerVolume  (spawn "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"))
  (() XF86AudioMute         (spawn "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"))
  (() XF86AudioPlay         (spawn "playerctl play-pause"))
  (() XF86AudioNext         (spawn "playerctl next"))
  (() XF86AudioPrev         (spawn "playerctl previous"))
  (() XF86MonBrightnessUp   (spawn "brightnessctl set +5%"))
  (() XF86MonBrightnessDown (spawn "brightnessctl set 5%-")))

;; Super with the left button drags a window, with the right button
;; resizes it. Either makes a tile float.
(defmouse
  (M left      move-window)
  (M right     resize-window))

;; ---------------------------------------------------------------------
;; Submaps: after (submap NAME) the next key, without modifiers, runs
;; one of these. Any other key leaves the submap.

(defsubmap session
  (l (spawn "swaylock -f"))
  (s (spawn "systemctl suspend"))
  (r restart)
  (e exit))

;; ---------------------------------------------------------------------
;; Rules. Matchers: (app-id "...") (title "...") (parent) for dialogs.
;; :glob true matches the pattern as a shell glob.

(defrules
  (float (parent)                    :center true)
  (float (app-id "pavucontrol")      :center true)
  (float (app-id "org.gnome.Calculator"))
  (float (title "Picture-in-Picture"))
  (float (title "*[Ss]ave [Aa]s*")    :glob true :center true)
  (ignore (app-id "showmethekey-gtk")))

;; ---------------------------------------------------------------------
;; Named scratchpads: the first press starts the program, later presses
;; show and hide its window. :geometry is (x y width height) as
;; fractions of the output.

(defscratchpads
  (term (spawn "foot --app-id scratch")
        (app-id "scratch")
        :geometry (0.1 0.1 0.8 0.8))
  (calc (spawn "qalculate-gtk")
        (app-id "qalculate-gtk")
        :geometry (0.3 0.3 0.4 0.4)))
