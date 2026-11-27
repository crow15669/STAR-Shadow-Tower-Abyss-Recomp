#!/usr/bin/env bash
# Stages "STAR settings.ini" for a commit with every value at the program's own (the table's fallbacks in
# src/star/options.cpp: the STAR preset, the rest as a fresh install has them), whatever the working copy says: the
# player's own settings, disc image and preset never go into git. Keep the list in step with options.cpp.
# Run from the project folder: bash tools/stage_settings.sh
set -euo pipefail
file="STAR settings.ini"
defaults=(
    resolution=3 fxaa=on smooth_60fps=on smooth_pictures=auto sharp_shadows=on near_shadows=on self_shadows=off
    shadow_walls=on flicker_filter=off glow=on
    text_fixes=on late_input=on analog_aim=on aim_turn_speed=100 aim_look_speed=100
    mouse_keyboard=on mouse_sensitivity=100 mouse_live_view=on mouse_invert_y=off gun_button=toggle
    melee_swings=buttons mouse_slowdown=full debug_menu=off record_pictures=off record_keep=5
    disc_image= preset= custom=
    "bind_move_forward=W, Up"
    "bind_move_back=S, Down"
    "bind_strafe_left=A, Left"
    "bind_strafe_right=D, Right"
    "bind_walk=LeftShift"
    "bind_fire=MouseLeft"
    "bind_gun=MouseRight"
    "bind_swing_view=MouseLeft"
    "bind_swing_flick=MouseRight"
    "bind_swing_right_left=MouseLeft"
    "bind_swing_left_right=MouseRight"
    "bind_stab=WheelUp"
    "bind_overhead=WheelDown"
    "bind_switch_weapons=R, Tab, Wheel"
    "bind_magic=Q, MouseMiddle"
    "bind_use_item=F"
    "bind_act=E, Space, Return, Keypad+Enter"
    "bind_menu=Escape, Backspace"
    "bind_pause=P"
)
script=(-e 's/\r$//')
for pair in "${defaults[@]}"; do
    name="${pair%%=*}"
    value="${pair#*=}"
    if [ -n "$value" ]; then
        script+=(-e "s/^${name} = .*\$/${name} = ${value}/")
    else
        script+=(-e "s/^${name} = .*\$/${name} =/")
    fi
done
hash=$(sed "${script[@]}" "$file" | git hash-object -w --stdin)
git update-index --cacheinfo 100644,"$hash","$file"
echo "staged $file with the default values ($hash)"
