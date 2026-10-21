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

## Building from source

Requirements: Linux, clang-cl and lld-link (LLVM), and the MSVC CRT and Windows SDK from [xwin](https://github.com/Jake-Shadle/xwin).

```
xwin --accept-license --arch x86 --cache-dir ~/.cache/xwin splat --output ~/.xwin
ashita/build.sh            # output: ashita/build/portfix.dll
ashita/build.sh install    # also copies it to Ashita/plugins. Refused while the game runs.
```

The Ashita build reads the SDK from the Ashita install. Environment variables `ASHITA_DIR`, `XWIN` and `LLVM_BIN` override the default paths.

`tools/package.sh <version>` builds the release archives into `dist/`.
