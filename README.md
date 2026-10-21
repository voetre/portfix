# portfix

An [Ashita v4](https://www.ashitaxi.com/) plugin that fixes graphics the PC port of FINAL FANTASY XI draws
wrong. Both effects were made for the PS2 version's renderer and carried over unchanged, so they break on PC
depth buffers and resolutions, on every GPU and on Windows as well as Wine/DXVK.

The fixes work on the game's own draw calls. portfix doesn't draw anything the game doesn't draw itself: it
changes the order, a texture or an offset the port got wrong.

## Fixes

- **Black water.** Bastok's canals (and other layered water) are solid black up close instead of dark teal. The
  water is three stacked planes about 0.03 units apart; the tint layer sits just under the ripple layer, and
  with the PC's 24-bit depth it fails the depth test behind it. portfix holds the ripple layer back and draws it
  after the tint, without writing depth.
- **Summon halos.** Carbuncle and the other summons get a thin, stair-stepped rim instead of the PS2's soft
  blue-violet halo. The game builds the halo from a 256x256 copy of the model's silhouette (16 screen pixels per
  texel at PC resolutions) and nudges its ghost copies by PS2-sized pixel offsets that are too small to see at
  PC resolutions. portfix draws those copies from the full-size silhouette and scales the offsets to the render
  width.

Both are on when the plugin loads.

## Commands

```
/portfix                     show what is on
/portfix water [on|off]      black water fix
/portfix glow [on|off|<n>]   summon halo fix; a number sets the ghost spacing scale (0 = render width / 640)
```

## Install

Copy `portfix.dll` into `Ashita/plugins/` and add `/load portfix` to your Ashita script (for example
`scripts/default.txt`). It needs no other plugin.

## Build

Cross-compiled on Linux with clang-cl against the MSVC CRT and Windows SDK from
[xwin](https://github.com/Jake-Shadle/xwin), so it uses the same C++ ABI as Ashita:

```
xwin --accept-license --arch x86 --cache-dir ~/.cache/xwin splat --output ~/.xwin   # once
./build.sh            # build/portfix.dll
./build.sh install    # also copies it into Ashita/plugins (refuses while the game runs)
```

`ASHITA_DIR`, `XWIN` and `LLVM_BIN` override the default paths.

[docs/NOTES.md](docs/NOTES.md) has how each bug was traced, and how close the halo fix is to the PS2.

## Credits

By voetre. MIT licence.
