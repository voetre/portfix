# Technical details

portfix intercepts Direct3D 8 draw calls made by FFXiMain.dll. The fix logic is in `core/fixes.cpp` and `core/disint.cpp`. Each launcher build forwards four device calls to it: DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP and Present. A fix either passes a draw through, holds it, or replaces it with an edited copy. The disintegrate fix also patches two places in FFXiMain.dll's code and makes draws of its own. Draws issued by portfix are marked so they are not processed a second time.

## Launcher builds

### Ashita v4

`ashita/src/portfix.cpp` is an Ashita plugin. It receives the device calls through the plugin Direct3D callbacks. Returning true from a callback skips the game's draw. The plugin priority is -10, so it runs before other plugins.

### Windower 4

Windower has no public plugin interface. `windower/portfix/portfix.lua` is a Lua addon. It loads `portfix.dll` with `package.loadlib` and calls `luaopen_portfix`. The dll takes the Lua functions it uses from `LuaCore.dll` at run time.

The dll hooks the device by patching four slots of its vtable: Present (15), DrawPrimitive (70), DrawIndexedPrimitive (71) and DrawPrimitiveUP (72). The device is the one the game uses. FFXiMain.dll keeps its renderer object in a global and the device at renderer + 8. The global is found by this byte pattern in FFXiMain.dll, at the code that creates the renderer:

```
68 78 04 00 00      push 478h
C6 44 24 0B 00      mov byte ptr [esp+0Bh], 0
E8 ?? ?? ?? ??      call operator new
83 C4 04            add esp, 4
85 C0               test eax, eax
0F 84 ?? ?? ?? ??   jz
8B C8               mov ecx, eax
E8 ?? ?? ?? ??      call constructor
85 C0               test eax, eax
A3 <global>         mov [global], eax
```

Under Windower the device is Windower's wrapper object. The patch applies to all objects of the wrapper class. Calls on other objects of that class pass through.

The dll does not create a Direct3D object or device. Launchers hook Direct3DCreate8 and treat a new device as the game's device. Under Ashita this reset Ashita's managers and stopped its command handling.

The patch is not removed. The dll pins itself so it stays loaded after the addon unloads. On unload the addon turns all fixes off. Fix state changes from Lua are applied on the render thread at the next Present.

The dll also exports `portfix_start` and `portfix_set(water, halo, disint)` for loading it without Lua.

## Black water

### Symptom

Layered water renders black near the camera. Distant water renders correctly. Seen in Port Bastok and Bastok Markets.

### Draw sequence

Each water surface is three flat planes drawn in this order. World heights are from Bastok Markets.

| # | Layer | Height | Draw | State |
|---|---|---|---|---|
| 1 | Depth darkening | 63.773 | DrawPrimitive | Untextured. TEXTUREFACTOR 96000000. Stage 0 colour MODULATE2X(CURRENT, TFACTOR), alpha MODULATE4X. Output is opaque black. Z-write on. |
| 2 | Ripples | 63.819 | DrawPrimitive | FVF XYZ, NORMAL, DIFFUSE, TEX1. Lighting off. Blend SRCALPHA, ONE. Stage 0 MODULATE(TEXTURE, TFACTOR). Texture transform COUNT2 (scrolling). Z-write on. |
| 3 | Tint | 63.796 | DrawPrimitive | Same vertex buffer as layer 1. TEXTUREFACTOR about 9619283x, changes each frame. ZFUNC LESSEQUAL. |

Layer 3 lies about 0.023 units below layer 2.

### Cause

Depth precision. At a distance of about 37 units the depth difference between layers 2 and 3 is about 7.8e-7 in z/w. A 24-bit depth buffer has steps of about 6e-8, so the difference is about 13 steps. Layer 3 fails the LESSEQUAL test against the depth written by layer 2. Only the black of layer 1 and faint ripples remain. A 16-bit depth buffer has steps of about 1.5e-5. At that precision layers 2 and 3 fall on the same depth value and layer 3 passes. Far from the camera the difference falls below one 24-bit step and the water renders correctly.

The layering was made for the PS2 renderer and was carried over to the PC version unchanged. No ZBIAS is used. Fog is not involved.

Forcing a 16-bit depth buffer was tested. It restores the tint at a distance, with banding, but not near the camera. Depth precision is the mechanism, but 16-bit depth alone does not reproduce the PS2 output. The PS2 renderer differs in at least one other respect: depth format, near plane or draw order. This is not known.

### Fix

1. Detect layer 2 by its state: FVF XYZ, NORMAL, DIFFUSE, TEX1, Z-write on, alpha blend on, SRCBLEND SRCALPHA, DESTBLEND ONE, lighting off, stage 0 texture transform COUNT2, stage 0 colour op MODULATE.
2. Record the full device state in a D3DSBT_ALL state block. Skip the draw.
3. Draw the next DrawPrimitive (layer 3) unchanged.
4. Save the current state, apply the recorded state, disable Z-write, draw layer 2, restore the saved state.

The resulting order is 1, 3, 2. A held draw is also released before the next DrawIndexedPrimitive or DrawPrimitiveUP. A held draw still pending at Present is dropped.

An earlier version drew layer 2 in place with Z-write off. Layer 3 then covered it fully, because its alpha saturates under MODULATE4X. The water showed flat tint with no ripples.

### Faithfulness

The fix is an approximation. The PS2 method of combining these layers is not known. The result was compared by eye with reference screenshots.

### Measured colour

Port Bastok, mean RGB of water regions.

| Mode | Far | Mid | Near |
|---|---|---|---|
| Fix off | 8,8,8 | 11,11,11 | 11,11,11 |
| 16-bit depth, fix off | 16,23,28 | 14,17,19 | 11,12,12 |
| Fix on | 25,35,42 | 28,38,45 | 28,38,45 |

## Summon halos

### Symptom

On the PS2, summons such as Carbuncle have a blue-violet halo around the silhouette, about 3 to 5 PS2 pixels wide, made of several offset copies of the outline. On the PC the same layer is a thin rim, stair-stepped in blocks of 4 to 6 pixels.

This halo is separate from the ground disc and sparkle effect (scheduler `efon`). That effect is started by the server and renders correctly on the PC.

### Draw sequence

1. Each mesh part is drawn lit, then drawn into a 4096x4096 silhouette texture.
2. One DrawPrimitiveUP quad copies the silhouette into a 256x256 render target. FVF XYZRHW, DIFFUSE, TEX1. Stride 28. Triangle strip, 2 primitives.
3. The 256x256 copy is drawn over the scene as full-screen quads:
   - Three ghost copies. Colour MODULATE2X, alpha MODULATE4X, blend SRCALPHA, INVSRCALPHA. Diffuse 1E0000FF, 0F6262FF, 140000FF. Each is offset by about 3.1 pixels in a pattern that rotates each frame. Example offsets: (1.17, 2.90), (4.06, -1.63), (-1.16, -2.90).
   - One opaque, alpha-tested copy at offset 0. Colour op SELECTARG2.
4. The full-size silhouette is drawn on top. Diffuse 80808080.

### Cause

Two values were carried over from the PS2 unchanged.

1. Copy resolution. At PC render sizes one texel of the 256x256 copy covers about 16 pixels. The opaque copy extends past the model edge as stair steps. Skipping the opaque copy removes the rim. Skipping the final silhouette shows the blocky copy alone.
2. Offset size. The offsets of about 3 pixels are in PS2 frame pixels. At 4096 pixels wide the ghost copies do not separate from the model.

### Fix

1. Detect the copy in step 2 by render target size 256x256. Record the source texture (the silhouette) and the 256x256 surface.
2. Detect the quads in step 3: DrawPrimitiveUP, stride 28, FVF XYZRHW, DIFFUSE, TEX1, triangle strip, 2 primitives, stage 0 texture whose level 0 surface is the recorded 256x256 surface.
3. Draw each of these quads with the full-size silhouette bound in place of the copy. Multiply the quad offset by render width / 640: each vertex moves by (x0, y0) * (k - 1), where (x0, y0) is the first vertex and k = width / 640.

Colours, blend states, ghost count and the offset pattern are the game's own.

### Faithfulness

This is a correction of two parameters in the game's own effect, not a reimplementation. Known differences from the PS2:

- The factor 640 assumes the offsets are in units of a 640-pixel-wide frame. The PS2 frame size for this effect was not verified.
- On the PS2 one texel of the 256x256 copy covers about 2.5 screen pixels. The low resolution softens the halo. With the full-size silhouette the halo edge is sharper than on the PS2. A closer match would keep a low-resolution copy at the same texel-to-pixel ratio, about 768x768 at 1920 pixels wide.
- The PS2 GS sampling and blending are not emulated.
- The result was compared by eye with PS2 recordings. It was not measured.

## Disintegrate

### Symptom

Some effects break a model up into pieces. The best known is the Shadow Lord's change of form in the Throne Room (cutscene 32004, effect file 16/98). On the PS2 the first form comes apart into dark textured shards that tumble away. Purple smoke rises from where they break off, with a faint heat haze. Over about 8 seconds the second form is left. On the PC there is a short purple burst. The first form stays whole and normally lit until it fades out. The same op is used by the Eald'Narche, Kam'lanaut and Selh'teus model changes.

Video, 20 seconds of cutscene 32004. Top: PS2. Bottom left: PC, fix off. Bottom right: PC, fix on.

<!-- docs/disintegrate-fix.mp4: upload to the GitHub README editor and put the attachment URL here -->

| PS2 | PC, fix off | PC, fix on |
|---|---|---|
| ![PS2](disintegrate-ps2.jpg) | ![PC, fix off](disintegrate-off.jpg) | ![PC, fix on](disintegrate-on.jpg) |

### Mechanism on the PS2

Effect 16/98 runs particles over the body. Each particle's per-frame op list includes op 0x65, with the record `65 07 00 00 | generator name | radius`. The radius is animated by the particle's op 0x6a keyframes: the sphere sits at the body's centre and grows from 0.045 to 4.25 over about 8 seconds.

1. Op 0x65 marks a sphere around the particle for the actor the effect plays on.
2. After skinning, every body triangle with a vertex inside a sphere is taken. The body stops drawing a taken triangle.
3. Each newly taken triangle is copied into a piece element (`disg`, generator mt00): its texture, UVs, positions and the body's lit vertex colours.
4. On frames that took triangles, generator g000 runs once at a taken triangle. It makes a heat haze element (`dist`) and, through its op 0x3c, the purple smoke (generator mt01).

Values read from the PS2 while the cutscene played:

- Pieces: life 40 ticks, rise 0.012 a tick, scale (0.8, 0.8, 1) shrinking to 0.02 over the life, a random spin up to (0.0314, 0.0332, 0.0332) radians a tick about x, y, z. One spin and rise per frame, shared by that frame's pieces. Colour: the three lit body colours times 0x80/0x80.
- Piece pass: depth test, no depth write, no blending, textured and Gouraud shaded.
- Haze: life 30 ticks, scale 0.4, turning 0.0698 radians a tick, rising 0.002 a tick. It is a 6-vertex fan, centre opaque and rim transparent, textured with the frame buffer. Each vertex samples the picture at its own position shifted by 0.07 in the element's frame. Centre alpha climbs, holds near 0x78/0x80 and falls. Blend (Cs - Cd) As + Cd, depth test, no depth write.

### Cause

On the PC, op 0x65's entry in the per-particle op table (dispatch in FUN_052da980) points at the do-nothing case shared with op 0x47. The PC has the piece element class (type 0x5A) but its update is a bare `ret`. Nothing is captured, nothing is hidden and no pieces are made. Only the effect's other sprites draw.

The haze element exists on the PC too, but the PC never registers its `dist` resource (a built-in on the PS2), so g000 makes no particles and its smoke never starts.

### Fix

portfix finds three functions in FFXiMain.dll by byte pattern and patches two places.

| | Pattern |
|---|---|
| Op dispatch (table address follows) | `8B 45 00 25 FF 00 00 00 48 83 F8 6D 0F 87 ?? ?? ?? ?? FF 24 85` |
| Particle world position | `83 EC 0C 56 8B F1 8D 46 54 8B 4E 54` |
| Generator emit (smoke; optional) | `83 EC 4C 53 55 56 8B F1 57 C6 44 24 13 00 F6 86 DC 00 00 00 01` |
| Part skinning (skin_part_b) | `83 EC 50 53 55 56 57 8B D9 6A 00 89 5C 24 20 E8` |

1. **Op 0x65.** The table entry is checked to equal op 0x47's, then pointed at a 32-byte stub. The stub calls a handler with the particle (EBX), the op record (EBP) and the generator (`[esp+48h]`), then jumps to the original case. The handler records a sphere: centre = the particle's world position, radius = record +0xc, actor = generator +0x3c. The actor is identified by its bone palette, `[[actor+0x6B8]+0x14]`.
2. **Skinning.** The body is skinned on the CPU into world space, one call per part. skin_part_b's `mov ecx,ebx / call <skin>` is redirected to a hook. For a part skinned with a capturing actor's palette (read from the global skin_part_b loads at +20), the hook points the skinning output at portfix's own buffer and copies it to the locked vertex buffer afterwards. This keeps a readable copy of the positions and normals without reading back write-combined memory.
3. **Capture.** On the next DrawIndexedPrimitive that draws from that vertex buffer, portfix reads the part's index range once and tests each triangle against this frame's spheres, as the PS2 does (any vertex closer than the radius). Each taken triangle becomes a piece: positions, UVs (stream 2), stage 0 texture, and colours lit with the draw's own lights and material, from the skinned normals. The part is then drawn from a portfix index buffer that leaves out the taken triangles. A part with nothing left is skipped. portfix draws every part of a capturing actor itself, with `D3DRS_ZBIAS` one step up, from its first frame, before anything is taken (see Depth below).
4. **Pieces.** Drawn once a frame, before the first interface draw (FVF XYZRHW, DIFFUSE, TEX1) on the scene target, with the PS2 values above. Depth test on, no depth write, no blending. Colour MODULATE2X(texture, diffuse), the PC's equivalent of the PS2's 0x80 = 1.0 vertex colour.
5. **Smoke.** On frames that took triangles, mt01 is emitted once per elapsed PS2 frame (30 Hz, up to 4) through the game's own emit function. The parent is a copy of the op 0x65 particle with its local position solved, by three probes of the world position function and Cramer's rule, so that it sits on a taken triangle. The smoke generator is found the way the PS2 finds it: record +8 (g000), then op 0x3c in g000's creation list.
6. **Haze.** The scene target is copied with CopyRects into a texture of the same size. Each haze element is drawn as the PS2's fan with that texture, with UVs from the projected, shifted vertex positions. Not drawn when the target is multisampled.

**Depth.** In cutscene 32004 the second form is a separate actor with the same mesh in the same pose. It is drawn after the first form, opaque, with depth test LESSEQUAL, so it wins every depth tie and would cover the first form from the moment the break-up starts. On the PS2 the first form stays on top and the second shows only through its holes. The one-step depth bias on the capturing actor gives the same result.

An actor with no spheres for 3 seconds gets its body back. Turning the fix off restores both patches.

### Faithfulness

The capture rule, piece motion, timings and draw states are the PS2's, read from its code and from memory while the cutscene played. Known differences:

- The PS2 tests the triangles one GS packet at a time, and once a vertex is inside, the rest of that packet goes too (about 4 triangles a packet). portfix has no packet layout for the PC's meshes and tests each triangle on its own, so a few more triangles stay on the body for a frame or two, and the shard clusters are smaller.
- The PC's meshes split the body into different triangles, so the shards are not the same shapes or number (about 2,050 triangles on the PC, 3,670 on the PS2). Both bodies are taken over the same 8 seconds.
- The PS2 lights the body per vertex in VU1. portfix recomputes the fixed-function lighting of the draw for the taken vertices. It is the PC's lit colour, not the PS2's.
- The haze copies the whole scene target once a frame, not the PS2 frame buffer at the moment of each draw.
- The pieces are drawn before the interface, not at the point in the frame the PS2 draws them. On both they draw over the burst and under the later smoke.
- 60 effect ticks a second and 30 PS2 frames a second are assumed for timing.
- Compared by eye with PS2 recordings, aligned on the effect's colour changes.

## Verification

Tested on the PC client under Wine using both Ashitav4 and Windower 4 with DXVK at 1920x1080. Not tested on Windows. The disintegrate fix has been tested on Ashita only (Throne Room cutscene 32004); it is built for Windower but not yet run there.

## Method

Draw calls were recorded per frame with render state, texture stage state, render target and DrawPrimitiveUP vertex data. Individual draws were skipped or given state overrides to identify each layer. PS2 references were recorded from the PS2 client running in PCSX2.
