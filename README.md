# unezQuake — Modernizing QuakeWorld Standards For Interested Parties
unezQuake is a fork of the ezQuake client that aims to bring more permissive standards and fun features to Quakeworld to help the game evolve in a positive way.

## Features
unezQuake has all of the latest ezQake features, plus:

 * `scr_autoid_ingame` - see autoID for teammates while playing  
 * `scr_scoreboard_showtracking` - see who spectators are following in the scoreboard (experimental)
 * `scr_scoreboard_scale` - size the scoreboard. Convenient when using high console resolutions.
 * `cl_autohop` - queue jumps automatically when holding +jump
 * `scr_teaminlay` - KTX independent teamoverlay with experimental MM2 enhancements
 * `cl_easyaircontrol` - experimental input handling for controller/joystick (only works in prewar)
 * `cl_autoshownick` - triggers shownick automatically when looking towards a teammate  
 * `cl_smartspawn` - uses jump to spawn even when pressing +attack  
 * `cl_safeswitch` - switches to next non-LG weapon in weapon order when entering water (does not prevent discharge)  
 * `cl_safeswitch_order "7 5 2 1"` - sets custom weapon order for cl_safeswitch (default is `""`)
 * `hud_[element]_border x` - sets a border of "x" thickness around any hud element  
 * `hud_[element]_border_color "R G B A"` - set color of border
 * `scr_teaminfo_mega_color` - change color of health in teamoverlay value if > 100 hp

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
