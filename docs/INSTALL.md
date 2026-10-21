# Installation

Downloads are on the [releases page](https://github.com/voetre/portfix/releases). Each release has one archive per launcher.

## Ashita v4

1. Download `portfix-ashita-<version>.zip`.
2. Extract it into the Ashita folder. This places `portfix.dll` in `Ashita/plugins/`.
3. Add this line to the Ashita startup script, for example `Ashita/scripts/default.txt`:

   ```
   /load portfix
   ```

4. Start the game. To load it without restarting, type `/load portfix` in game.

Check: `/portfix` lists each fix as on.

Remove: type `/unload portfix`, delete the line from the script and delete `Ashita/plugins/portfix.dll`.

## Windower 4

1. Download `portfix-windower-<version>.zip`.
2. Extract it into the Windower folder. This places `portfix.lua` and `portfix.dll` in `Windower4/addons/portfix/`.
3. Load it in game: `//lua load portfix`.
4. To load it at each start, add this line to `Windower4/scripts/init.txt`. The addon list in the Windower launcher shows only official addons. portfix does not appear there.

   ```
   lua load portfix
   ```

Check: `//portfix` lists each fix as on.

Remove: type `//lua unload portfix`, delete the line from `init.txt` and delete `Windower4/addons/portfix/`.

`portfix.dll` stays loaded until the game closes. After `//lua unload portfix` the fixes are off. To replace the dll with a new version, close the game first.

## Building from source

Both builds run on Linux.

Ashita: clang-cl and lld-link (LLVM), and the MSVC CRT and Windows SDK from [xwin](https://github.com/Jake-Shadle/xwin). The build reads the Ashita SDK from the Ashita install.

```
xwin --accept-license --arch x86 --cache-dir ~/.cache/xwin splat --output ~/.xwin
ashita/build.sh            # output: ashita/build/portfix.dll
ashita/build.sh install    # also copies it to Ashita/plugins. Refused while the game runs.
```

Environment variables `ASHITA_DIR`, `XWIN` and `LLVM_BIN` override the default paths.

Windower: mingw-w64 for i686 (`i686-w64-mingw32-g++`).

```
windower/build.sh          # output: windower/build/portfix/
windower/build.sh install  # also copies it to Windower4/addons/portfix. Refused while the game runs.
```

Environment variables `WINDOWER_DIR` and `CXX` override the default paths.

`tools/package.sh <version>` builds the release archives into `dist/`.
