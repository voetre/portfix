# portfix

Fixes for graphics that the PC version of FINAL FANTASY XI draws incorrectly. The affected effects were written for the PS2 renderer and were not adapted to PC depth buffers and resolutions.

All fixes are on by default. The water and halo fixes only change the order, texture or position of draws the game already makes. The disintegrate fix restores a PS2 effect the PC skips: it patches two places in the game's code and draws the effect's pieces itself.

Builds: Ashita v4 plugin, Windower 4 addon.

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
| Before and After |
|---|
| ![Carbuncle, fix off on the left, fix on on the right](media/halo-off-on.jpg) |

| Comparison Video |
|---|
| https://github.com/user-attachments/assets/8b68e54f-139e-4321-b4e2-ae63424262ec |
| PS2 at 3x resolution, PC with no fix, PC with portfix |

### Disintegrate

Some effects break a model up, most notably the Shadow Lord's change of form in the Throne Room. On the PS2 the first form comes apart into dark shards with purple smoke. On the PC it stays whole. The fix restores the break-up, smoke and haze.

| PS2 | PC, fix off | PC, fix on |
|---|---|---|
| ![PS2](docs/disintegrate-ps2.jpg) | ![PC, fix off](docs/disintegrate-off.jpg) | ![PC, fix on](docs/disintegrate-on.jpg) |

<!-- Video: docs/disintegrate-fix.mp4 (PS2 top, PC fix off bottom left, PC fix on bottom right). Upload through the GitHub editor and put the attachment URL here. -->

## Commands

Ashita: `/portfix`. Windower: `//portfix` in chat, `portfix` in the console.

| Command | Action |
|---|---|
| `/portfix` | List fixes and their state. |
| `/portfix water [on\|off]` | Black water fix. No argument toggles. |
| `/portfix halo [on\|off]` | Summon halo fix. No argument toggles. |
| `/portfix disint [on\|off]` | Disintegrate effect fix. No argument toggles. |
| `/portfix help` | List commands. |

Settings are not saved. All fixes are on each time the plugin loads.

## Licence

MIT.
