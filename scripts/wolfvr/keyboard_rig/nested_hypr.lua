-- [VR KEYBOARD 2026-11-05] a bare Hyprland nested in the desktop's, for test viewers only (session_vrkeys.sh WL=1): the
-- viewer is its only window, so it has the keyboard there, and wtype's real Wayland key presses reach it without ever
-- touching the desktop's own focus. Nothing is started from here.
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = "1" })
hl.config({ animations = { enabled = false } })
hl.config({ misc = { force_default_wallpaper = 0, disable_hyprland_logo = true } })
-- as Paul's desktop (Omarchy looknfeel.lua: misc.focus_on_activate = true): a window asking for activation gets focus
hl.config({ misc = { focus_on_activate = true } })
