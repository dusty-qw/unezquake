# unezQuake Changelog

## Version 2.0.2 - 2026-06-21

- More bug fixes around weapon switching
- Updated client grenade timing
- Disabled prediction while dead, tracking, or during intermission
- Better separation of legacy projectile behavior from EZCSQC
- Buffered weapon animations to synchronize them with projectiles and sounds
- Fixed issues with `cl_weaponpreselect`

## Version 2.0.1 - 2026-06-19

- Fixed weapon switching and impulse binds breaking prediction
- Resolved issues when using the axe
- Fixed LG problems including prediction not working and sounds becoming delayed or desynchronized
- Fixed prediction problems when toggling the console

## Version 2.0 - 2026-06-18

### EZCSQC Antilag

Improved antilag support overhauls weapon prediction, knockback smoothing, audio,
and explosion prediction to improve responsiveness.

Projectile prediction no longer fires a fake projectile and attempts to blend it
with the server projectile. The server instead adopts the client's predicted
entity, eliminating the visual transition.

**NOTE: antilag projectile timing has changed to be faithful to classic antilag 2
(no more 10 ms lead). This means projectiles on older versions of unezquake tuned
for reki/andeh's antilag 1 implementation might not look as smooth on EZCSQC antilag
servers. Bottom line: use the latest client for the smoothest experience.**

unezQuake remains backwards compatible with legacy antilag 1 servers and preserves
the original prediction behavior through the classic `cl_predict_*` cvars. These
cvars provide extended functionality when connected to a supported EZCSQC antilag
server.

- `cl_predict_smoothview 1` uses a half-life decay algorithm on EZCSQC servers instead of simple linear smoothing. It is also platform-aware.
- `cl_predict_sound 1` replaces `cl_predict_jump` and predicts local movement feedback sounds such as jumping, landing, and water splashes.
- `cl_predict_explosions 1` predicts sound and movement kick for high-confidence self explosions.
- `cl_predict_legacy 0` mimics legacy prediction timing at pings over 80 ms.
- `cl_pext_ezcsqc 1` controls use of the new EZCSQC antilag support.

Supported servers advertise `ezcsqc 1` in serverinfo. The client prints
`EZCSQC Antilag ready` after setup completes.

## Version 1.4.2 - 2026-06-02

This release contains important spray decal fixes that break compatibility with
versions 1.4.0 and 1.4.1. Version 1.3.x clients are unaffected. MVDSV and KTX
should also be upgraded.

- Fixed images being reloaded from disk with every placement
- Fixed images being rescaled with every placement
- Reduced chunk size to better accommodate message length
- Improved QTV and demo tolerance for non-contiguous chunks
- Fixed spray spam evicting other players' decals
- Suppressed spray sounds when receiving spray backfill messages
- Loaded and cached the spray image directory on startup

## Version 1.4.1 - 2026-05-25

- Added sounds for spray decals

## Version 1.4.0 - 2026-05-23

### Spray Decals

unezQuake now supports spray decals. Place a `spray.png` image in `qw/sprays`
and bind a key to `+spray`. The maximum resolution is 128x128; larger images are
downscaled by the client.

Only pixel data is shared between the client and server. Decals are cached and
indexed to minimize bandwidth usage.

- `cl_spray_show` controls spray opacity and visibility
- `cl_spray_size` controls the in-game spray size
- `cl_spray_distance` controls how close the player must be to a surface
- `cl_spray_colorize` tints sprays with a specified RGB color

### Other Updates

- Changed `vid_postprocess_text` to default to `1`, matching ezQuake
- Added `allow_f_system` back to the client
- Fixed broken user cvars related to custom nicknames
- Fixed `cl_safestrafe` frame calculations

## Version 1.3.8 - 2026-01-25

- Renamed `vid_gammafontfix` to `vid_postprocess_text`
- Fixed underlying text being drawn over cursors and modal dialogs in the console and server browser

## Version 1.3.7 - 2026-01-22

### Tracker and Nicknames

- Added `r_tracker_iconoverlay 1` to display powerup icons over weapon icons in the VX Tracker
- Allowed user IDs in `/nick` match strings
- Added `scr_scoreboard_nick_names` to toggle nickname overrides on the scoreboard
- Added `scr_scoreboard_nick_color` to distinguish nickname overrides from in-game names
- Applied nickname overrides to chat, notify, tracking, and other HUD elements

### Other Updates

- Added `vid_gammafontfix` to prevent gamma and contrast changes from affecting console fonts and charsets
- Simplified prediction scaling and added `cl_predict_scale_threshold`
- Added the `hud_total_strength` HUD element to display combined health and armor
- Changed the map vote HUD element to disappear ten seconds after the last vote
- Fixed alpha mismatches in the corners of elements using `border_radius`

## Version 1.3.6 - 2025-12-09

### Nickname Overrides

Added nickname override groups for players who use multiple names or long clan
tags:

```text
/nick dusty "dust0r, pierre, iceburn"
/nick xero "ToT_xero"
```

Use `/nicklist` to show all entries, `/nickedit <name>` to edit an entry, and
`/nick_clear <name>` to remove an entry. Custom nicknames are saved by
`cfg_save`.

## Version 1.3.5 - 2025-11-03

- Added a security fix from Slime to block circumvention of remote capability checks
- Added `cl_delay_input` to delay mouse input by a specified number of milliseconds for testing
- Added the `hud_mapvote_*` element to display the name and thumbnail of the map being voted on
- Fixed phantom rockets when projectile prediction is disabled
- Fixed the implementation of custom acceleration curves

## Version 1.3.4 - 2025-09-01

- Preserved the original `m_accel` behavior by adding acceleration type `0` (`NONE`)
- Added `scr_allowsnap` reporting to `f_ruleset`; a value of `0` reports `refusesnap`
- Added client-side `cl_safestrafe`

The added `m_accel` type shifted the custom Raw Accel curve values up by one.
For example, the Jump curve moved from `3` to `4`.

## Version 1.3.3 - 2025-08-24

### Raw Accel

- Added native Raw Accel integration with selectable curves through `m_accel_type`
- Added curve-specific parameters through `m_accel_[type]_*`
- Added `m_accel_smooth` and `m_accel_smooth_halflife`
- Added `m_accel_legacy` to select legacy acceleration calculations or Raw Accel's gain method

### Blob Explosions

- Added `r_explosion_color`
- Added `r_explosion_scale`
- Added `r_explosion_sparks`

These cvars customize blob explosions produced by `r_explosionType 6`.

## Version 1.3.2 - 2025-08-09

### Individual Teammate Skin Colors

Added `r_teammateskincolors_enable` and
`r_teammateskincolor[1-6] "r g b name1 name2 {name3} ..."`. Teammates whose
names contain a configured substring receive the corresponding color. Curly
braces specify an exact, case-insensitive match.

```text
r_teammateskincolor1 "0 255 0 jim {bob} john"
```

### HUD Element Borders and Radius

Added the `border_radius` property to groups and other HUD elements. It accepts
one or more radius values and can be used with or without a border.

```text
hud_group4_border_radius 5
hud_gameclock_border_radius 5 5 0 0
```

### Player Prediction

- Changed `cl_predict_lerp` to use an error-distance threshold
- Added `cl_predict_show_errors` to print prediction errors above a specified distance

### Other Updates

- Improved demo playback of Wipeout games
- Removed the dependency between `cl_autoshownick` and `tp_point` settings

## Version 1.3.1 - 2025-07-28

Improved player prediction and interpolation for players using iDrive or unstable
network connections.

- Added `cl_predict_velocity_scale` values `1` through `3` for increasingly aggressive velocity-based prediction scaling
- Added `cl_predict_lerp 0.026` to interpolate player movement prediction errors; `0` disables it

## Version 1.3.0 - 2025-07-26

- Added `cl_model_height` to customize the height of models and simple items
- Added seamless spectator tracking across map changes
- Fixed spawn marker model outlines blocking the point of view from inside the model
- Fixed broken or missing chat icons after the alpha update

## Version 1.2.5 - 2025-05-09

Merged the latest ezQuake changes:

- Merged ezQuake pull requests [#1046](https://github.com/QW-Group/ezquake-source/issues/1046), [#1043](https://github.com/QW-Group/ezquake-source/issues/1043), [#1042](https://github.com/QW-Group/ezquake-source/issues/1042), [#1044](https://github.com/QW-Group/ezquake-source/issues/1044), and [#1045](https://github.com/QW-Group/ezquake-source/issues/1045)
- Added `hud_ammo[1-4]_text_color_{low,normal}`
- Added `cl_window_caption 3`
- Initialized results to `NULL` so they are always safe to free

## Version 1.2.4 - 2025-04-08

- Merged the latest ezQuake changes

## Version 1.2.3 - 2025-02-20

- Included the latest ezQuake updates
- Fixed bugs in the classic scoreboard

## Version 1.2.2 - 2025-01-11

- Reverted Slime's previous remote code execution fix in favor of the new implementation

## Version 1.2.1 - 2024-12-16

- Fixed `cfg_save` removing aliases

## Version 1.2.0 - 2024-12-14

This release addresses a security issue that allowed servers unrestricted access
to the client's console.

- Added `cl_allow_remote_commands` to control which aliases and commands a remote server may execute
- Added `cl_allow_downloads` to control whether a server may upload files to the Quake directory

Disabling `cl_allow_downloads` also prevents the client from using the `download`
command. Because servers can no longer create aliases for clients, maps must be
selected with `votemap <mapname>` unless the user creates a local map alias.

## Version 1.1.6 - 2024-11-01

- Added `scr_scoreboard_scale`
- Added `scr_teaminfo_mega_color` for teammate health above 100
- Replaced `+pogo` with the reportable `cl_autohop` cvar
- Fixed crashes when `scr_scoreboard_showtracking` was enabled

## Version 1.1.5 - 2024-10-17

- Disabled experimental spectator tracking information by default

## Version 1.1.4 - 2024-03-11

- Added `scr_autoid_ingame "1"` to enable limited teammate auto-ID while playing
- Added `scr_autoid_ingame_weapon "1"` to show a teammate's best weapon
- Added `scr_autoid_ingame_namelength "6"` to limit auto-ID name length
- Added `scr_autoid_yoffset "0"` to adjust auto-ID text vertically

## Version 1.1.3 - 2024-01-31

- Added `r_tracker_colorfix "1"` to color the full frag line using the tracker good or bad colors
- Added `cl_safeswitch "1"` to switch to the best available non-LG weapon when entering water
- Added `cl_safeswitch_order` to configure the weapon order used by `cl_safeswitch`
- Fixed issues related to scoreboard spectator tracking
- Re-enabled `cl_easyaircontrol` during standby

## Version 1.1.2 - 2023-11-02

- Updated the ezQuake base through version 3.6.4

## Version 1.1.1 - 2023-10-27

Added KovaaK's experimental `cl_easyaircontrol`. Suggested values range from
`45` to `90`, with `62.5` as a starting point and `0` disabling the feature.
It is available only during standby mode.

## Version 1.1.0 - 2023-06-13

### Scoreboard Spectator Tracking

Added scoreboard tracking information with the following cvars:

- `scr_scoreboard_showtracking`
- `scr_scoreboard_showtracking_format`
- `scr_scoreboard_showtracking_scale`
- `scr_scoreboard_showtracking_namewidth`
- `scr_scoreboard_showtracking_x`
- `scr_scoreboard_showtracking_y`

### Outlines

- Added `gl_outline_width` to configure player and model outline thickness
- Added `r_fx_geometry_width`
- Added `r_fx_geometry_color`
- Added `r_fx_geometry_factor`
- Added `r_fx_geometry_factor2`

### HUD Borders

- Added `hud_[element]_border`
- Added `hud_[element]_border_color`

### Rulesets

Removed rulesets from unezQuake. The client is permissive but reports settings
such as scripts, inlay, `cl_hud`, iDrive, modifications, and triggers through
`f_ruleset`. Clients without reportable settings return `clear`.

This and future releases include the Reki antilag and prediction cvars by default.

## Version 1.0.9 - 2023-05-04

- Updated the ezQuake base to version 3.6.3

## Version 1.0.8 - 2022-11-14

No release notes were published.

## Version 1.0.7a - 2022-08-11

No release notes were published.

## Version 1.0.6a-r402 - 2022-04-25

- Included the latest prediction cvars from [Reki's fork](https://github.com/Iceman12k/ezquake-source)

## Versions 1.0.4 and 0.9.4 - 2021-03-05

- Limited `+pogo` to the default ruleset
- Reduced the `cl_rollangle` limit from `10` to `5` to match ezQuake

## Version 1.0.3.a - 2021-01-14

- Added `/teaminlay`, a server-independent team overlay with additional messaging enhancements
- Documented that `cl_autoshownick` requires `teammate` in `tp_point`
- Prevented ruleset output when its value does not change
- Changed `cl_autoshownick` checks from four to twenty times per second

## Version 0.9.3 - 2021-01-14

The 0.9.x releases track ezQuake's stable 3.2.x branch.

- Documented that `cl_autoshownick` requires `teammate` in `tp_point`
- Prevented ruleset output when its value does not change
- Prevented team inlay from sending negative health values
- Added `scr_teaminlay_low_health`

## Version 1.0.2 - 2020-12-12

- Added `cl_smartspawn` to prevent weapons firing while spawning with `+attack`
- Added `cl_autoshownick` to automatically use `/shownick` when pointing at teammates
- Added `+pogo` for continuous jumping
- Added `scr_sbar_drawarmor666` to display armor while carrying the pentagram with the classic HUD
- Allowed rulesets to be changed without disconnecting

## Version 0.9.2 - 2020-12-12

The 0.9.x releases track ezQuake's stable 3.2.x branch.

- Added `/teaminlay`, a server-independent team overlay with additional messaging enhancements
- Added `cl_smartspawn` to prevent weapons firing while spawning with `+attack`
- Added `cl_autoshownick` to automatically use `/shownick` when pointing at teammates
- Added `+pogo` for continuous jumping
- Added `scr_sbar_drawarmor666` to display armor while carrying the pentagram with the classic HUD
- Allowed rulesets to be changed without disconnecting

## Version 1.0.1 - 2020-10-10

- Updated the ezQuake base to version 3.6-alpha3

## Version 0.9.0 - 2020-09-21

- Based on ezQuake 3.2.2

## Version 1.0 - 2020-09-20

- Based on ezQuake 3.6
