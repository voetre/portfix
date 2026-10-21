# portfix notes

How each bug was found and fixed. Both fixes live in src/portfix.cpp. They started in Toy Box (2026-10-21) and
moved here the same day; the dev tools named below (`/tb drawstate`, `drawskip`, `drawset`, `costume`) are Toy
Box's.

## Black water (WaterSurface, 2026-10-21)

Bastok's canals (and other layered water) showed solid black up close instead of dark teal. Found with Toy Box's
dev draw tools (Debug page: `/tb drawstate`, `drawskip tf`, `drawset`). The water is three flat planes at the
same height, offset about 0.03 units, drawn in this order:

1. Depth darkening: DP 1415 prims, untextured, TEXTUREFACTOR 96000000, stage 0 MODULATE2X(CURRENT,TFACTOR) with
   alpha MODULATE4X, so the vertex alpha (0..128) saturates: an opaque black layer. z-write on.
2. Ripple surface: DP 2847 prims, additive (SRCALPHA/ONE), scrolling texture (TEXTURETRANSFORMFLAGS COUNT2),
   stage 0 MODULATE(TEXTURE,TFACTOR), z-write on, FVF XYZ|NORMAL|DIFFUSE|TEX1, lighting off.
3. Teal tint: same VB as 1, TEXTUREFACTOR 9619283x (drifts each frame), ZFUNC LESSEQUAL, about 0.03 below 2.

Cause: depth precision. At ~37 units the layers differ by about 7.8e-7 in z/w. The game's D24S8 depth has
steps of ~6e-8 (24-bit integer; DXVK on AMD uses 32-bit float, finer still), so the gap is ~13 steps: 3 fails
the depth test behind 2 and only the black of 1 (plus faint ripples) shows. Only coarse depth (16-bit, steps of
~1.5e-5) puts the planes at the same depth so 3 passes LESSEQUAL; far away the gap shrinks below a step and the
water looks right. Not a DXVK/AMD bug: the user reports it on Windows and NVIDIA too. The layering looks made for
the PS2 version's renderer and carried over to the PC port, where it has been black up close all along. No ZBIAS
is used; fog isn't involved (water fog end is 14391). (An earlier version of this note blamed DXVK's float depth
and said 24-bit integer depth merges the layers: wrong, 24-bit separates them too.)

Fix: hold 2 back (matched by WaterSurface: the states above). Its whole device state goes into a D3DSBT_ALL
state block, and it is drawn right after the next draw (3, the tint), with z-write off; DIP/DPUP flush it
first if one comes before, and Present drops it. So the order becomes 1, 3, 2: tint, then ripples added on top,
like the user's reference picture. The first version only drew 2 with z-write off in place: 3 then covered it
completely (its alpha saturates, MODULATE4X), so the water was a flat blue-teal with no ripples (user report,
2026-10-21). The plugin draws inside the DrawPrimitive hook (m_Drawing guard) and returns true, since Ashita
draws after the callback. On by default; `/portfix water on|off`. Tested in
Bastok Markets at night (ripples visible on the teal). Untested elsewhere (other zones' water, other GPUs).

16-bit depth test (Toy Box Debug page "16-bit depth (test)", `/tb depth16`; 2026-10-21, Port
Bastok by the drawbridge, water fix off, camera raised and tilted down). Water colour, mean RGB:
fix off: far 8,8,8 / mid 11,11,11 / near 11,11,11 (black); 16-bit depth: far 16,23,28 / mid 14,17,19 / near
11,12,12; water fix: 25,35,42 / 28,38,45 / 28,38,45. So coarser depth brings the tint back from the far side, with
banding, but not up close: precision is the mechanism, yet 16-bit alone doesn't reproduce the intended look. The
PS2 renderer must have drawn the layers differently in some other way too (depth format, near plane or order;
unknown). Nothing else changed with 16-bit depth in four directions there (diffs only show clouds, chat, idle
animation), so no other stacked-layer effect was found in that spot.

## Summon halos (GlowDraw, 2026-10-21)

The PC (with or without the efon aura, a separate server-triggered effect) draws Carbuncle and other summons with
a thin stair-stepped rim where the PS2 shows a soft halo.

PS2 vs PC side by side (2026-10-21, a real SMN summon on the PS2 server vs costume 16 on the PC): the PS2 shows
efon (pale ground disc + sparkles) AND a separate outline halo: a soft blue-violet band about 3-5 PS2 pixels wide
around the whole silhouette, built from several offset copies of the outline. The PC (with or without `/tb anim
efon`) has the same layer but only as a thin cyan rim, stair-stepped in 4-6 px blocks with soft edges, i.e. a
low-res copy of the model stretched up, not expanded and not tinted.

Traced with `/tb drawstate` (DPUP quads now dumped as `up [x,y colour u,v]`): each mesh part is drawn lit, then
into a 4096x4096 silhouette texture. One DPUP quad (stride 28, FVF XYZRHW|DIFFUSE|TEX1) copies that down to a
256x256 render target. The 256 copy is then drawn back over the scene as full-screen quads: three ghosts
(MODULATE2X colour, MODULATE4X alpha, SRCALPHA/INVSRCALPHA; diffuse 1E0000FF, 0F6262FF, 140000FF) nudged ~3.1 px
in a pattern that turns each frame, e.g. (1.17,2.90) (4.06,-1.63) (-1.16,-2.90); one opaque alpha-tested copy at
offset 0 (SELECTARG2); then the full-size silhouette itself (80808080). Two port bugs: the 256 copy is 16 scene
pixels per texel, so the opaque copy sticks out past the model as stair steps (skipping it removes the rim; skipping
the last quad shows the blocky copy alone), and the nudges are PS2 frame pixels kept as-is, so at 4096 the ghosts
never separate. Fix (`/portfix glow`, on by default, GlowDraw): draw those
quads from the full-size silhouette instead of the 256 copy and scale the nudges by render width / 640
(`/portfix glow <n>` sets the scale). Result: blue ghost copies around the model, matching the PS2.

How faithful it is: the fix keeps the game's own draw calls, colours, blend states, ghost count and offset
pattern, and changes two inputs. The 640 in the offset scale assumes the nudges are in 640-wide PS2 frame pixels
(unchecked). Drawing from the full-size silhouette removes the stair steps but gives crisper ghosts than the PS2,
where the 256 copy was about 2.5 screen pixels per texel and so softened the band. A closer match would keep a
low-res copy sized to that ratio (about 768x768 at 1920 wide). Compared by eye against PS2 captures only.
