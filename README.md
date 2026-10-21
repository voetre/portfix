# portfix

Fixes for graphics that the PC version of FINAL FANTASY XI draws incorrectly. The affected effects were written for the PS2 renderer and were not adapted to PC depth buffers and resolutions. The faults occur on all GPUs, on Windows and on Linux.

All fixes are on by default. portfix only changes the order, texture or position of draws the game already makes. It adds no draws of its own.

Builds: Ashita v4 plugin.

Installation: [docs/INSTALL.md](docs/INSTALL.md).
Technical details: [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Fixes

### Black water

Layered water, for example in Port Bastok and Bastok Markets, renders black near the camera. The fix restores the intended colour.

| Off | On |
|---|---|
| ![Water, fix off](media/water-off.jpg) | ![Water, fix on](media/water-on.jpg) |

### Summon halos

Summons such as Carbuncle are drawn with a stair-stepped outline. The PS2 version draws a soft outline halo. The fix restores the halo.

![Carbuncle, fix off on the left, fix on on the right](media/halo-off-on.jpg)

Left: off. Right: on.

Video, PS2 then PC with the fix off then on, for each summon: [media/halo-fix.mp4](media/halo-fix.mp4).

## Commands

| Command | Action |
|---|---|
| `/portfix` | List fixes and their state. |
| `/portfix water [on\|off]` | Black water fix. No argument toggles. |
| `/portfix halo [on\|off]` | Summon halo fix. No argument toggles. |
| `/portfix help` | List commands. |

Settings are not saved. All fixes are on each time the plugin loads.

## Licence

MIT. Author: voetre.
