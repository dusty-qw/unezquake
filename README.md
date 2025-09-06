# unezQuake — Modernizing QuakeWorld Standards For Interested Parties
unezQuake is a fork of the ezQuake client that aims to bring more permissive standards and fun features to Quakeworld to help the game evolve in a positive way.

## Features
unezQuake has all of the latest ezQuake features, plus:

### Antilag Support

 * `cl_predict_buffer` - how many frames are buffered before prediction is used. `"0"` is the fastest but will result in more errors like phantom rockets
 * `cl_predict_jump` - controls if your jump sound is predicted
 * `cl_predict_projectiles` - controls if projectiles are predicted
 * `cl_predict_smoothview` - values between 0.1 and 2 will attempt to smooth fast position changes from packet loss or knockback
 * `cl_predict_weaponsound` - controls prediction of weapon sounds

### Improved Player Prediction / Movement

 * `cl_predict_lerp` - interpolate errors in player prediction
 * `cl_predict_show_errors` - Prints prediction errors for other players
 * `cl_predict_velocity_scale` - reduces player jitter while strafing (accepts values 0-3, 3 being most aggressive)

### New Gameplay Features

 * `cl_autohop` - queue jumps automatically when holding +jump
 * `cl_autoshownick` - triggers shownick automatically when looking towards a teammate  
 * `cl_easyaircontrol` - experimental input handling for controller/joystick (only works in prewar)
 * `cl_model_height` - ability to set custom height of models/simpleitems
 * `cl_safestrafe` - use SOCD without violating rules or triggering detection
 * `cl_safeswitch` - switches to a non-LG weapon when entering water (does not prevent discharge)
    * `cl_safeswitch_order "7 5 2 1"` - set a custom weapon order (default is `""`)
 * `cl_smartspawn` - uses jump to spawn even when pressing +attack
 * `r_teammateskincolors_enable` - enable custom skin colors for teammates
    * `r_teammateskincolor[1-6] "R G B substring1 substring2 ..."` - any teammate matching the substrings will be colored accordingly

### Additional Input Support

 * `cl_delay_input` - Simulate input lag - Delay mouse input in milliseconds
 * `m_accel_type` - Native Raw Accel integration. Use your favorite accel curve and settings
    * `m_accel_smooth` - EMA-based input smoothing applied before accel curves
    * `m_accel_smooth_halflife` - Alternative time-based smoothing using exponential decay
    * `m_accel_cap_type` - Specify if sens caps should apply to input or output (default)
    * `m_accel_distance_mode` - Distance calculation method
    * `m_accel_legacy` - Use legacy accel calculations or Raw Accel's "Gain" method

### New EyeCandy Customizations

 * `r_explosion_color` - Set color of blob explosions (r_explosionType 6)
 * `r_explosion_scale` - Set size of blob explosions
 * `r_explosion_sparks` - Show or hide explosion sparks of blob explosions

### Hud and Scoreboard Enhancements

 * `hud_[element]_border x` - sets a border of "x" thickness around any hud element  
    * `hud_[element]_border_color "R G B A"` - set color of border
    * `hud_[element]_border_radius` - give your hud elements rounded corners
 * `scr_autoid_ingame` - see autoID for teammates while playing
    * `scr_autoid_ingame_namelength`
    * `scr_autoid_ingame_weapon`
    * `scr_autoid_ingame_armor_health`
 * `scr_scoreboard_scale` - size the scoreboard. Convenient when using high console resolutions
 * `scr_scoreboard_showtracking` - see who spectators are following in the scoreboard (experimental)
    * `scr_scoreboard_showtracking_format`
    * `scr_scoreboard_showtracking_namewidth`
    * `scr_scoreboard_showtracking_scale`
    * `scr_scoreboard_showtracking_x`
    * `scr_scoreboard_showtracking_y`
 * `scr_teaminfo_mega_color` - change color of health in teamoverlay value if > 100 hp
 * `scr_teaminlay` - KTX independent teamoverlay with experimental MM2 enhancements

### Other Miscellaneous Fixes

 * `auto_retrack` - spectator tracking persists across map changes
 * Model outlines aren't drawn if POV is inside of a model (such as a spawn marker)
 * No more rulesets - client directly reports controversial features and "clear" if none are used

## Support

Need help with using unezQuake? Contact Dusty in #dev-corner on [discord][discord]

## Compiling

On Linux, `./build-linux.sh` produces an ezQuake binary in the top directory. 

For a more in-depth description of how to build on all platforms, have a look at 
[BUILD.md](BUILD.md).


 [nQuake]: http://nquake.com/
 [webchat]: http://webchat.quakenet.org/?channels=#ezquake
 [IRC]: irc://irc.quakenet.org/#ezquake
 [forum]: http://www.quakeworld.nu/forum/8
 [qtv]: http://qtv.quakeworld.nu/
 [nightly]: https://builds.quakeworld.nu/ezquake/snapshots/
 [releases]: https://github.com/ezQuake/ezquake-source/releases
 [issues]: https://github.com/ezQuake/ezquake-source/issues
 [homepage]: https://ezquake.com
 [discord]: http://discord.quake.world/
