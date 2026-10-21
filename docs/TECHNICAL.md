# Technical details

portfix intercepts Direct3D 8 draw calls made by FFXiMain.dll. The fix logic is in `core/fixes.cpp`. Each launcher build forwards four device calls to it: DrawPrimitive, DrawIndexedPrimitive, DrawPrimitiveUP and Present. A fix either passes a draw through, holds it, or replaces it with an edited copy. Draws issued by portfix are marked so they are not processed a second time.

## Black water

### Symptom

Layered water renders black near the camera. Distant water renders correctly. Seen in Port Bastok and Bastok Markets. Present on Windows and Linux, with AMD and NVIDIA GPUs. It is not specific to Wine or DXVK.

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

## Verification

Tested on the PC client under Wine with DXVK at 1920x1080. Black water: Port Bastok, Bastok Markets. Summon halos: Carbuncle, Fenrir, Ifrit, Titan, Leviathan, Garuda, Shiva, Ramuh, Diabolos, Cait Sith, Fire Spirit, Promathia. Not tested on Windows.

## Method

Draw calls were recorded per frame with render state, texture stage state, render target and DrawPrimitiveUP vertex data. Individual draws were skipped or given state overrides to identify each layer. PS2 references were recorded from the PS2 client running in PCSX2.
