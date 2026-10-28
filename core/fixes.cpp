#include "fixes.h"
#include "debuglog.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

bool Fixes::OnDrawPrimitive(const D3DPRIMITIVETYPE t, const UINT start, const UINT count)
{
    if (m_Drawing || m_Device == nullptr)
        return false;
    if (m_Disint)
        DisintBeforeDraw();
    if (!m_Water)
        return false;
    if (WaterSurface()) // the ripple layer waits for the tint layer
    {
        WaterFlush();
        return WaterHold(t, start, count);
    }
    const bool layer = WaterLayer();
    if (!m_WaterHeld && !layer)
        return false;
    WaterDraw(t, start, count, layer); // a held ripple layer goes on top of this one
    WaterFlush();
    return true;
}

bool Fixes::OnDrawIndexedPrimitive(const D3DPRIMITIVETYPE t, const UINT minIndex, const UINT numVerts, const UINT startIndex, const UINT count)
{
    if (m_Drawing || m_Device == nullptr)
        return false;
    if (m_WaterHeld)
        WaterFlush();
    if (m_Disint)
    {
        DisintBeforeDraw();
        if (DisintDraw(t, minIndex, numVerts, startIndex, count))
            return true;
    }
    return m_Shine && ShineDraw(t, minIndex, numVerts, startIndex, count);
}

bool Fixes::OnDrawPrimitiveUP(const D3DPRIMITIVETYPE t, const UINT count, const void* data, const UINT stride)
{
    if (m_Drawing || m_Device == nullptr)
        return false;
    if (m_Disint)
        DisintBeforeDraw();
    if (m_WaterHeld)
        WaterFlush();
    return m_Halo && stride == 28 && HaloDraw(t, count, data);
}

void Fixes::OnPresent(void)
{
    m_WaterHeld = false; // a ripple draw with nothing after it in the frame is dropped
    m_WaterState.Clear(); // and its references let go: none may outlive the frame (a device reset needs them gone)
    m_HaloSrc   = nullptr;
    m_HaloLow   = nullptr;
    if (m_Disint)
        DisintPresent();
}

void Fixes::SetWater(const bool on)
{
    m_Water = on;
    if (!on)
        WaterRelease();
}

const char* Fixes::SetDisint(const bool on)
{
    if (on == m_Disint)
        return nullptr;
    if (!on)
    {
        DisintRemove();
        m_Disint = false;
        return nullptr;
    }
    if (m_Device == nullptr)
        return "no device yet";
    const char* error = DisintInstall();
    m_Disint          = error == nullptr;
    PF_DEBUG("disint: %s%s", error == nullptr ? "on" : "couldn't start: ", error == nullptr ? "" : error);
    return error;
}

// What a report needs about the setup: portfix's state, the graphics card and which Direct3D 8 is loaded (the game's
// own, DXVK, dgVoodoo...), the back buffer and the capabilities the fixes check.
void Fixes::DebugReport(const char* version) const
{
    PF_DEBUG("portfix %s: water %s, halo %s, shine %s (split %s), disint %s", version, m_Water ? "on" : "off", m_Halo ? "on" : "off",
             m_Shine ? "on" : "off", m_Split ? "on" : "off", m_Disint ? "on" : "off");
    for (const char* dll : {"d3d8.dll", "d3d9.dll", "d3d11.dll", "d3d12.dll", "dxgi.dll", "wined3d.dll"})
        if (HMODULE m = ::GetModuleHandleA(dll))
        {
            char path[MAX_PATH] = "?";
            ::GetModuleFileNameA(m, path, MAX_PATH);
            // the file's own description and version tell the game's d3d8 from DXVK, dgVoodoo and other wrappers
            std::string about;
            DWORD handle = 0;
            const DWORD size = ::GetFileVersionInfoSizeA(path, &handle);
            std::vector<char> info(size);
            if (size != 0 && ::GetFileVersionInfoA(path, 0, size, info.data()))
            {
                struct Lang { WORD lang, page; }* langs = nullptr;
                UINT n = 0;
                if (::VerQueryValueA(info.data(), "\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &n) && n >= sizeof(Lang))
                    for (const char* field : {"FileDescription", "ProductName", "FileVersion"})
                    {
                        char key[80];
                        std::snprintf(key, sizeof(key), "\\StringFileInfo\\%04x%04x\\%s", langs[0].lang, langs[0].page, field);
                        char* value = nullptr;
                        UINT len    = 0;
                        if (::VerQueryValueA(info.data(), key, reinterpret_cast<void**>(&value), &len) && len > 1)
                            about += std::string(about.empty() ? "" : ", ") + value;
                    }
            }
            PF_DEBUG("module %s: %s%s%s%s", dll, path, about.empty() ? "" : " (", about.c_str(), about.empty() ? "" : ")");
        }
    if (m_Device == nullptr)
    {
        PF_DEBUG("no Direct3D device yet");
        return;
    }
    IDirect3D8* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(m_Device->GetCreationParameters(&cp)) && SUCCEEDED(m_Device->GetDirect3D(&d3d)) && d3d != nullptr)
    {
        D3DADAPTER_IDENTIFIER8 id{};
        if (SUCCEEDED(d3d->GetAdapterIdentifier(cp.AdapterOrdinal, 0, &id)))
            PF_DEBUG("adapter %u: %s (vendor %04lx device %04lx, driver %s %lu.%lu)", cp.AdapterOrdinal, id.Description, id.VendorId, id.DeviceId, id.Driver,
                     id.DriverVersion.HighPart, id.DriverVersion.LowPart);
        d3d->Release();
    }
    PF_DEBUG("device: type %d, behaviour flags %08lx", cp.DeviceType, cp.BehaviorFlags);
    IDirect3DSurface8* bb = nullptr;
    if (SUCCEEDED(m_Device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb != nullptr)
    {
        D3DSURFACE_DESC d{};
        bb->GetDesc(&d);
        PF_DEBUG("back buffer %ux%u format %d, multisample %d", d.Width, d.Height, d.Format, d.MultiSampleType);
        bb->Release();
    }
    IDirect3DSurface8* ds = nullptr;
    if (SUCCEEDED(m_Device->GetDepthStencilSurface(&ds)) && ds != nullptr)
    {
        D3DSURFACE_DESC d{};
        ds->GetDesc(&d);
        PF_DEBUG("depth buffer %ux%u format %d", d.Width, d.Height, d.Format);
        ds->Release();
    }
    D3DCAPS8 caps{};
    if (SUCCEEDED(m_Device->GetDeviceCaps(&caps)))
        PF_DEBUG("caps: vs %lx ps %lx, blend stages %lu, simultaneous textures %lu, TSSARGTEMP %d, MODULATE4X %d, cube maps %d, max texture %lux%lu",
                 caps.VertexShaderVersion, caps.PixelShaderVersion, caps.MaxTextureBlendStages, caps.MaxSimultaneousTextures,
                 (caps.PrimitiveMiscCaps & D3DPMISCCAPS_TSSARGTEMP) != 0, (caps.TextureOpCaps & D3DTEXOPCAPS_MODULATE4X) != 0,
                 (caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP) != 0, caps.MaxTextureWidth, caps.MaxTextureHeight);
}

void Fixes::Release(void)
{
    SetSplit(false);
    SetDisint(false);
    WaterRelease();
    SpotRelease();
}

void Fixes::WaterRelease(void)
{
    m_WaterHeld = false;
    m_WaterState.Clear();
    m_WaterKeep.Clear();
}

namespace
{
    // what portfix's draws set or depend on: everything the water layers and disintegrate's passes touch
    const D3DRENDERSTATETYPE DrawRs[] = {
        D3DRS_ZENABLE, D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
        D3DRS_CULLMODE, D3DRS_ZFUNC, D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_DITHERENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_FOGENABLE,
        D3DRS_SPECULARENABLE, D3DRS_ZBIAS, D3DRS_TEXTUREFACTOR, D3DRS_LIGHTING, D3DRS_AMBIENT, D3DRS_COLORVERTEX, D3DRS_BLENDOP,
        D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_RANGEFOGENABLE, D3DRS_FOGVERTEXMODE,
        D3DRS_NORMALIZENORMALS, D3DRS_DIFFUSEMATERIALSOURCE, D3DRS_AMBIENTMATERIALSOURCE, D3DRS_SPECULARMATERIALSOURCE,
        D3DRS_EMISSIVEMATERIALSOURCE, D3DRS_COLORWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_CLIPPING, D3DRS_WRAP0, D3DRS_WRAP1,
        D3DRS_VERTEXBLEND, D3DRS_INDEXEDVERTEXBLENDENABLE};
    const D3DTEXTURESTAGESTATETYPE DrawTss[] = {
        D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX,
        D3DTSS_ADDRESSU, D3DTSS_ADDRESSV, D3DTSS_MAGFILTER, D3DTSS_MINFILTER, D3DTSS_MIPFILTER, D3DTSS_MIPMAPLODBIAS, D3DTSS_MAXMIPLEVEL,
        D3DTSS_TEXTURETRANSFORMFLAGS, D3DTSS_COLORARG0, D3DTSS_ALPHAARG0, D3DTSS_RESULTARG};
    static_assert(sizeof(DrawRs) / sizeof(DrawRs[0]) <= 48 && sizeof(DrawTss) / sizeof(DrawTss[0]) <= 20, "DrawState arrays too small");
}

void Fixes::DrawState::Capture(IDirect3DDevice8* d)
{
    Clear();
    for (size_t i = 0; i < sizeof(DrawRs) / sizeof(DrawRs[0]); ++i)
        d->GetRenderState(DrawRs[i], &rs[i]);
    for (DWORD stage = 0; stage < 2; ++stage)
        for (size_t i = 0; i < sizeof(DrawTss) / sizeof(DrawTss[0]); ++i)
            d->GetTextureStageState(stage, DrawTss[i], &tss[stage][i]);
    d->GetTransform(D3DTS_WORLD, &world);
    d->GetTransform(D3DTS_VIEW, &view);
    d->GetTransform(D3DTS_PROJECTION, &proj);
    d->GetTransform(D3DTS_TEXTURE0, &tex0);
    d->GetTransform(D3DTS_TEXTURE1, &tex1);
    for (DWORD stage = 0; stage < 4; ++stage)
        d->GetTexture(stage, &texture[stage]); // these hold a reference until Clear
    d->GetStreamSource(0, &stream, &stride);
    d->GetVertexShader(&shader);
    d->GetPixelShader(&pixelShader);
    held = true;
}

void Fixes::DrawState::Apply(IDirect3DDevice8* d) const
{
    if (!held)
        return;
    for (size_t i = 0; i < sizeof(DrawRs) / sizeof(DrawRs[0]); ++i)
        d->SetRenderState(DrawRs[i], rs[i]);
    for (DWORD stage = 0; stage < 2; ++stage)
        for (size_t i = 0; i < sizeof(DrawTss) / sizeof(DrawTss[0]); ++i)
            d->SetTextureStageState(stage, DrawTss[i], tss[stage][i]);
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetTransform(D3DTS_VIEW, &view);
    d->SetTransform(D3DTS_PROJECTION, &proj);
    d->SetTransform(D3DTS_TEXTURE0, &tex0);
    d->SetTransform(D3DTS_TEXTURE1, &tex1);
    for (DWORD stage = 0; stage < 4; ++stage)
        d->SetTexture(stage, texture[stage]);
    d->SetStreamSource(0, stream, stride);
    d->SetVertexShader(shader);
    d->SetPixelShader(pixelShader);
}

void Fixes::DrawState::Clear(void)
{
    for (auto*& t : texture)
        if (t != nullptr)
            t->Release(), t = nullptr;
    if (stream != nullptr)
        stream->Release(), stream = nullptr;
    held = false;
}

// Water is several flat layers a few hundredths of a yalm apart, drawn after the scene with LESSEQUAL. The PS2
// composites them in draw order; on the PC two things reject layers that should show, and the water goes black:
// - Port Bastok's sea: a black layer, the scrolling ripple layer added on top (writes depth), then a dark tint on
//   the black layer's geometry just under the ripples. The tint fails against the ripples' depth.
// - Bastok Markets' canals: the zone's own opaque water plane (dark tiles, drawn with the scene) lies 0.0415 above
//   the water layers. The game draws the water pass with a near plane of 0.1003 instead of the scene's 0.1, which
//   pulls it 0.3% of its distance closer; along a view ray the gap is 0.0415 * distance / camera height, so the
//   water only wins with the camera more than ~14 yalms above it. Lower, the whole canal goes black at once.
// The fix holds the ripple draw back (its state kept by hand) and draws it after the tint, with no depth
// write: tint, then ripples added on top (drawing them in place with no depth write showed flat tint only). And it
// draws every scrolling water layer WaterLift higher, so no layer sits under the plane it's painted on, at any
// camera height or distance.
// The ripple layer: FVF XYZ|NORMAL|DIFFUSE|TEX1, unlit, additive (SRCALPHA, ONE), writes depth, scrolling texture.
bool Fixes::WaterSurface(void) const
{
    DWORD fvf = 0, v = 0;
    m_Device->GetVertexShader(&fvf);
    if (fvf != (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1))
        return false;
    static const std::pair<D3DRENDERSTATETYPE, DWORD> rs[] = {
        {D3DRS_ZWRITEENABLE, TRUE}, {D3DRS_ALPHABLENDENABLE, TRUE}, {D3DRS_SRCBLEND, D3DBLEND_SRCALPHA}, {D3DRS_DESTBLEND, D3DBLEND_ONE}, {D3DRS_LIGHTING, FALSE}};
    for (const auto& [s, want] : rs)
        if (m_Device->GetRenderState(s, &v), v != want)
            return false;
    m_Device->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &v);
    if (v != D3DTTFF_COUNT2)
        return false;
    m_Device->GetTextureStageState(0, D3DTSS_COLOROP, &v);
    return v == D3DTOP_MODULATE;
}

// Any scrolling water layer (ripples, tints, shore layers): FVF XYZ|NORMAL|DIFFUSE|TEX1, unlit, alpha blended,
// stage 0 texture coordinates scrolled by a texture transform.
bool Fixes::WaterLayer(void) const
{
    DWORD fvf = 0, v = 0;
    m_Device->GetVertexShader(&fvf);
    if (fvf != (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1))
        return false;
    if (m_Device->GetRenderState(D3DRS_LIGHTING, &v), v != FALSE)
        return false;
    if (m_Device->GetRenderState(D3DRS_ALPHABLENDENABLE, &v), v != TRUE)
        return false;
    m_Device->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &v);
    return v == D3DTTFF_COUNT2;
}

// Draws with the current state; a water layer is moved WaterLift up first. FFXI's y points down and the water's
// world matrix includes the view, so its second row is world down in view space.
void Fixes::WaterDraw(const D3DPRIMITIVETYPE t, const UINT start, const UINT count, const bool lift)
{
    D3DMATRIX w, up;
    float len = 0.0f;
    if (lift && SUCCEEDED(m_Device->GetTransform(D3DTS_WORLD, &w)))
        len = std::sqrt(w._21 * w._21 + w._22 * w._22 + w._23 * w._23);
    if (len > 0.0f)
    {
        DebugLog::Once("water-lift", "water: scrolling water layer lifted %.2f (%u primitives)", WaterLift, count);
        up = w;
        up._41 -= WaterLift * w._21 / len;
        up._42 -= WaterLift * w._22 / len;
        up._43 -= WaterLift * w._23 / len;
        m_Device->SetTransform(D3DTS_WORLD, &up);
    }
    m_Drawing = true;
    RawDrawPrimitive(t, start, count);
    m_Drawing = false;
    if (len > 0.0f)
        m_Device->SetTransform(D3DTS_WORLD, &w);
}

bool Fixes::WaterHold(const D3DPRIMITIVETYPE t, const UINT start, const UINT count)
{
    DebugLog::Once("water-hold", "water: ripple layer held back to draw after the tint (%u primitives)", count);
    m_WaterState.Capture(m_Device);
    m_WaterType  = t;
    m_WaterStart = start;
    m_WaterCount = count;
    m_WaterHeld  = true;
    return true;
}

void Fixes::WaterFlush(void)
{
    if (!m_WaterHeld)
        return;
    m_WaterHeld = false;
    m_WaterKeep.Capture(m_Device);
    m_WaterState.Apply(m_Device);
    m_Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    WaterDraw(m_WaterType, m_WaterStart, m_WaterCount, true);
    m_WaterKeep.Apply(m_Device);
    m_WaterKeep.Clear();
    m_WaterState.Clear();
}

// Outline halos (Carbuncle's blue aura, other summons): the model is drawn into a 4096x4096 silhouette texture,
// copied down to 256x256, then the copy is drawn back over the scene as full-screen quads: three alpha-blended
// tinted ghosts nudged ~3 px in a pattern that turns each frame, one opaque alpha-tested copy, then the silhouette
// itself on top. The 256 copy is 16 scene pixels a texel, so the opaque copy sticks out past the model as stair
// steps, and ~3 px nudges (PS2 frame pixels, kept as-is) don't separate the ghosts at 4096.
// The fix draws those quads from the full-size silhouette and scales the nudges by render width / 640.
// Quads: XYZRHW|DIFFUSE|TEX1 (stride 28), a 4-vertex strip from (dx, dy) to (w + dx, h + dy).
bool Fixes::HaloDraw(const D3DPRIMITIVETYPE t, const UINT count, const void* data)
{
    DWORD fvf = 0;
    m_Device->GetVertexShader(&fvf);
    if (fvf != (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1) || t != D3DPT_TRIANGLESTRIP || count != 2)
        return false;
    IDirect3DBaseTexture8* tex = nullptr;
    m_Device->GetTexture(0, &tex);
    if (tex == nullptr)
        return false;
    tex->Release(); // the stage still holds it
    IDirect3DSurface8* rt = nullptr;
    D3DSURFACE_DESC d{};
    if (FAILED(m_Device->GetRenderTarget(&rt)) || rt == nullptr)
        return false;
    rt->GetDesc(&d);
    if (d.Width == 256 && d.Height == 256) // the copy-down: remember the silhouette and the copy's surface
    {
        m_HaloSrc = tex;
        m_HaloLow = rt;
        rt->Release();
        return false;
    }
    rt->Release();
    if (m_HaloSrc == nullptr || tex->GetType() != D3DRTYPE_TEXTURE)
        return false;
    IDirect3DSurface8* s0 = nullptr;
    if (FAILED(static_cast<IDirect3DTexture8*>(tex)->GetSurfaceLevel(0, &s0)) || s0 == nullptr)
        return false;
    s0->Release();
    if (s0 != m_HaloLow)
        return false;
    BYTE v[4 * 28];
    std::memcpy(v, data, sizeof(v));
    float x0, y0;
    std::memcpy(&x0, v, 4);
    std::memcpy(&y0, v + 4, 4);
    const float k = static_cast<float>(d.Width) / 640.0f;
    for (int i = 0; i < 4; ++i)
    {
        float xy[2];
        std::memcpy(xy, v + i * 28, 8);
        xy[0] += x0 * (k - 1.0f);
        xy[1] += y0 * (k - 1.0f);
        std::memcpy(v + i * 28, xy, 8);
    }
    m_Device->SetTexture(0, m_HaloSrc);
    DebugLog::Once("halo", "halo: first halo quad drawn from the full-size silhouette (%ux%u)", d.Width, d.Height);
    m_Drawing = true;
    RawDrawPrimitiveUP(t, count, v, 28);
    m_Drawing = false;
    m_Device->SetTexture(0, tex);
    return true;
}

// Sphere-mapped shine (Warlord Rojgnoj's gold plates, and other models with an 'evev' texture). The PS2 draws the
// model, then adds the sphere map in a second pass, Cs * Ad + Cd: the map times the batch's strength / 255 times the
// frame alpha the first pass left, which is the model texture's alpha on the GS scale (0x80 = 1.0, 0xFF ~ 2.0), at
// uv = 0.5 + 0.5 * (nx, -ny) of the camera-space normal.
// The port does it in one draw: stage 1 adds the map times the stage-0 alpha (texture alpha * strength / 255 on the
// 0xFF = 1.0 scale: half the PS2's), at uv = (nx, ny) through an identity matrix, clamped, so a face turned to the
// camera takes the map's dark corner instead of its bright middle.
// The fix redraws it with the PS2's mapping and twice the alpha: stage 1 puts map * 2 * alpha in TEMP (its alpha
// result is the game's, current + map alpha), stage 2 adds TEMP to the model's colour.
bool Fixes::ShineCaps(void)
{
    if (m_ShineTemp < 0)
    {
        D3DCAPS8 caps{};
        m_ShineTemp = SUCCEEDED(m_Device->GetDeviceCaps(&caps)) && (caps.PrimitiveMiscCaps & D3DPMISCCAPS_TSSARGTEMP) != 0 && caps.MaxTextureBlendStages >= 3 ? 1 : 0;
        m_SpotOps   = (caps.TextureOpCaps & D3DTEXOPCAPS_MODULATE4X) != 0 && (caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP) != 0;
        PF_DEBUG("shine: device caps: TSSARGTEMP %d, blend stages %lu, MODULATE4X %d, cube maps %d -> sphere-map fix %s, highlight fix %s",
                 (caps.PrimitiveMiscCaps & D3DPMISCCAPS_TSSARGTEMP) != 0, caps.MaxTextureBlendStages, (caps.TextureOpCaps & D3DTEXOPCAPS_MODULATE4X) != 0,
                 (caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP) != 0, m_ShineTemp == 1 ? "on" : "unavailable", m_ShineTemp == 1 && m_SpotOps ? "on" : "unavailable");
    }
    return m_ShineTemp == 1;
}

bool Fixes::ShineDraw(const D3DPRIMITIVETYPE t, const UINT minIndex, const UINT numVerts, const UINT startIndex, const UINT count)
{
    DWORD v = 0;
    if (m_Device->GetTextureStageState(1, D3DTSS_COLOROP, &v), v != D3DTOP_MODULATEALPHA_ADDCOLOR)
        return false;
    if (m_Device->GetTextureStageState(1, D3DTSS_COLORARG1, &v), v != D3DTA_CURRENT)
        return false;
    if (m_Device->GetTextureStageState(1, D3DTSS_COLORARG2, &v), v != D3DTA_TEXTURE)
        return false;
    if (m_Device->GetTextureStageState(1, D3DTSS_TEXCOORDINDEX, &v), v == (D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR | 1))
        return ShineCaps() && m_SpotOps && SpotDraw(t, minIndex, numVerts, startIndex, count);
    if (v != (D3DTSS_TCI_CAMERASPACENORMAL | 1))
        return false;
    if (!ShineCaps())
        return false;

    static const D3DMATRIX ps2 = {0.5f, 0.0f, 0.0f, 0.0f, 0.0f, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.0f, 1.0f};
    static const struct
    {
        DWORD stage;
        D3DTEXTURESTAGESTATETYPE type;
        DWORD value;
    } set[] = {
        {1, D3DTSS_COLOROP, D3DTOP_MODULATE2X}, {1, D3DTSS_COLORARG1, D3DTA_TEXTURE}, {1, D3DTSS_COLORARG2, D3DTA_CURRENT | D3DTA_ALPHAREPLICATE},
        {1, D3DTSS_RESULTARG, D3DTA_TEMP}, {2, D3DTSS_COLOROP, D3DTOP_ADD}, {2, D3DTSS_COLORARG1, D3DTA_CURRENT}, {2, D3DTSS_COLORARG2, D3DTA_TEMP},
        {2, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1}, {2, D3DTSS_ALPHAARG1, D3DTA_TEMP}, {3, D3DTSS_COLOROP, D3DTOP_DISABLE}, {3, D3DTSS_ALPHAOP, D3DTOP_DISABLE}};
    DWORD keep[sizeof(set) / sizeof(set[0])];
    D3DMATRIX keepTm;
    m_Device->GetTransform(D3DTS_TEXTURE1, &keepTm);
    for (size_t i = 0; i < sizeof(set) / sizeof(set[0]); ++i)
    {
        m_Device->GetTextureStageState(set[i].stage, set[i].type, &keep[i]);
        m_Device->SetTextureStageState(set[i].stage, set[i].type, set[i].value);
    }
    m_Device->SetTransform(D3DTS_TEXTURE1, &ps2);
    m_Drawing = true;
    RawDrawIndexedPrimitive(t, minIndex, numVerts, startIndex, count);
    m_Drawing = false;
    m_Device->SetTransform(D3DTS_TEXTURE1, &keepTm);
    for (size_t i = sizeof(set) / sizeof(set[0]); i-- > 0;)
        m_Device->SetTextureStageState(set[i].stage, set[i].type, keep[i]);
    return true;
}

// Reflection-vector shine (weapons, armour: most of the PS2's shiny gear). The PS2 adds a highlight in a second pass,
// Cs * Ad + Cd: its 64x64 spot texture times the light's colour times strength / 256 (vertex colour, 0x80 = 1.0)
// times the first pass's alpha (texture alpha, 0x80 = 1.0), at the sphere-map position of the world reflection
// vector in a frame whose axis is the light: brightest when the eye's reflection points at the light, fading
// out over about 45 degrees (SpotSpread: radius 0.6 x sin(angle / 2)).
// The port draws it with stage 1 MODULATEALPHA_ADDCOLOR of a cube ("cubemap spec"), the camera-space reflection
// vector through a texture matrix diag(1, -1, -1) x camera-to-world x the light's view. Three things differ:
// - the diag(1, -1, -1) turns the reflection upside down, so the cube's spot (its -Z face, the light's way back)
//   is found when the reflection points away from the light, at the floor under a lamp;
// - the spot face is the PS2 spot texture pasted onto one face, which halves its spread (a face's tan(angle)
//   against the sphere map's sin(angle / 2));
// - it's white and at half strength: the stage-0 alpha is the PS2 alpha on the 0xFF = 1.0 scale, and the light's
//   colour isn't in it.
// The fix draws it with the matrix's flip taken out, a cube rebuilt from the game's own spot face with the PS2's
// spread, and stage 1 = cube x 4 x TFACTOR into TEMP, with TFACTOR's colour set to half light 0's diffuse (the
// light the game aims the cube at), so TEMP is cube x light x 2; stage 2 adds TEMP x the stage-0 alpha. TFACTOR's
// alpha (strength / 2, used by stage 0) and the stages' alpha results are the game's.
void Fixes::SpotRelease(void)
{
    if (m_SpotCube != nullptr)
        m_SpotCube->Release();
    m_SpotCube = nullptr;
    m_SpotFrom = nullptr;
}

bool Fixes::SpotBuild(IDirect3DCubeTexture8* game)
{
    SpotRelease();
    D3DSURFACE_DESC gd{};
    if (FAILED(game->GetLevelDesc(0, &gd)) || (gd.Format != D3DFMT_A8R8G8B8 && gd.Format != D3DFMT_X8R8G8B8) || gd.Width < 8)
        return false;
    // the game's spot: its -Z face, a copy of the PS2's sphere-mapped spot texture
    const UINT n = gd.Width;
    D3DLOCKED_RECT lr{};
    if (FAILED(game->LockRect(D3DCUBEMAP_FACE_NEGATIVE_Z, 0, &lr, nullptr, D3DLOCK_READONLY)))
        return false;
    DWORD* spot = new DWORD[n * n];
    for (UINT y = 0; y < n; ++y)
        std::memcpy(spot + y * n, static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch, n * 4);
    game->UnlockRect(D3DCUBEMAP_FACE_NEGATIVE_Z, 0);
    auto sample = [spot, n](const float u, const float v) -> DWORD {  // bilinear, black outside
        const float x = u * n - 0.5f, y = v * n - 0.5f;
        const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
        const float fx = x - x0, fy = y - y0;
        float acc[4]{};
        for (int k = 0; k < 4; ++k)
        {
            const int xi = x0 + (k & 1), yi = y0 + (k >> 1);
            const float w = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
            const DWORD c = xi >= 0 && yi >= 0 && xi < static_cast<int>(n) && yi < static_cast<int>(n) ? spot[yi * n + xi] : (spot[0] & 0xff000000);
            for (int b = 0; b < 4; ++b)
                acc[b] += w * static_cast<float>(c >> (8 * b) & 0xff);
        }
        DWORD out = 0;
        for (int b = 0; b < 4; ++b)
            out |= static_cast<DWORD>(std::lround(acc[b]) & 0xff) << (8 * b);
        return out;
    };
    IDirect3DCubeTexture8* cube = nullptr;
    if (FAILED(m_Device->CreateCubeTexture(SpotSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube)) || cube == nullptr)
    {
        delete[] spot;
        return false;
    }
    for (int face = 0; face < 6; ++face)
    {
        if (FAILED(cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face), 0, &lr, nullptr, 0)))
            continue;
        for (UINT y = 0; y < SpotSize; ++y)
            for (UINT x = 0; x < SpotSize; ++x)
            {
                // the direction of this texel (Direct3D's cube face layout)
                const float a = 2.0f * (x + 0.5f) / SpotSize - 1.0f, b = 2.0f * (y + 0.5f) / SpotSize - 1.0f;
                float d[3];
                switch (face)
                {
                    case 0: d[0] = 1, d[1] = -b, d[2] = -a; break;
                    case 1: d[0] = -1, d[1] = -b, d[2] = a; break;
                    case 2: d[0] = a, d[1] = 1, d[2] = b; break;
                    case 3: d[0] = a, d[1] = -1, d[2] = -b; break;
                    case 4: d[0] = a, d[1] = -b, d[2] = 1; break;
                    default: d[0] = -a, d[1] = -b, d[2] = -1; break;
                }
                const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                const float c   = -d[2] / len; // cos of the angle from the spot's axis (-Z)
                const float r   = SpotSpread * std::sqrt((std::max)(0.0f, (1.0f - c) * 0.5f)); // 0.6 sin(angle / 2)
                const float side = std::sqrt(d[0] * d[0] + d[1] * d[1]);
                // the same way round as the game's face near the axis: face -Z texel (u, v) is direction (-u, -v)
                const float u = side > 1e-6f ? 0.5f - r * d[0] / side : 0.5f, v = side > 1e-6f ? 0.5f - r * d[1] / side : 0.5f;
                *reinterpret_cast<DWORD*>(static_cast<BYTE*>(lr.pBits) + y * lr.Pitch + x * 4) = sample(u, v);
            }
        cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), 0);
    }
    delete[] spot;
    m_SpotCube = cube;
    m_SpotFrom = game;
    return true;
}

bool Fixes::SpotDraw(const D3DPRIMITIVETYPE t, const UINT minIndex, const UINT numVerts, const UINT startIndex, const UINT count)
{
    IDirect3DBaseTexture8* tex = nullptr;
    if (FAILED(m_Device->GetTexture(1, &tex)) || tex == nullptr)
        return false;
    tex->Release(); // the stage still holds it
    if (tex->GetType() != D3DRTYPE_CUBETEXTURE)
        return false;
    if (tex != m_SpotFrom)
    {
        const bool built = SpotBuild(static_cast<IDirect3DCubeTexture8*>(tex));
        PF_DEBUG(built ? "shine: highlight cube built from the game's (%p)" : "shine: couldn't build the highlight cube from the game's (%p); the game draws it",
                 static_cast<void*>(tex));
        if (!built)
            return false;
    }

    // light 0's colour, halved so it fits TFACTOR (x 4 at stage 1 makes it light x 2)
    D3DLIGHT8 light{};
    BOOL lit = FALSE;
    float rgb[3] = {1.0f, 1.0f, 1.0f};
    if (SUCCEEDED(m_Device->GetLightEnable(0, &lit)) && lit && SUCCEEDED(m_Device->GetLight(0, &light)))
        rgb[0] = light.Diffuse.r, rgb[1] = light.Diffuse.g, rgb[2] = light.Diffuse.b;
    DWORD tf = 0;
    m_Device->GetRenderState(D3DRS_TEXTUREFACTOR, &tf);
    DWORD tfColour = tf & 0xff000000;
    for (int i = 0; i < 3; ++i)
        tfColour |= static_cast<DWORD>(std::lround((std::min)(1.0f, (std::max)(0.0f, rgb[i] * 0.5f)) * 255.0f)) << (16 - 8 * i);

    static const struct
    {
        DWORD stage;
        D3DTEXTURESTAGESTATETYPE type;
        DWORD value;
    } set[] = {
        {1, D3DTSS_COLOROP, D3DTOP_MODULATE4X}, {1, D3DTSS_COLORARG1, D3DTA_TEXTURE}, {1, D3DTSS_COLORARG2, D3DTA_TFACTOR},
        {1, D3DTSS_RESULTARG, D3DTA_TEMP}, {2, D3DTSS_COLOROP, D3DTOP_MODULATEALPHA_ADDCOLOR}, {2, D3DTSS_COLORARG1, D3DTA_CURRENT}, {2, D3DTSS_COLORARG2, D3DTA_TEMP},
        {2, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1}, {2, D3DTSS_ALPHAARG1, D3DTA_TEMP}, {3, D3DTSS_COLOROP, D3DTOP_DISABLE}, {3, D3DTSS_ALPHAOP, D3DTOP_DISABLE}};
    DWORD keep[sizeof(set) / sizeof(set[0])];
    D3DMATRIX keepTm, tm;
    m_Device->GetTransform(D3DTS_TEXTURE1, &keepTm);
    tm = keepTm;
    for (int c = 0; c < 4; ++c) // diag(1, -1, -1) x matrix: rows 2 and 3 negated, the flip taken out
        (&tm._21)[c] = -(&keepTm._21)[c], (&tm._31)[c] = -(&keepTm._31)[c];
    for (size_t i = 0; i < sizeof(set) / sizeof(set[0]); ++i)
    {
        m_Device->GetTextureStageState(set[i].stage, set[i].type, &keep[i]);
        m_Device->SetTextureStageState(set[i].stage, set[i].type, set[i].value);
    }
    m_Device->SetTransform(D3DTS_TEXTURE1, &tm);
    m_Device->SetRenderState(D3DRS_TEXTUREFACTOR, tfColour);
    m_Device->SetTexture(1, m_SpotCube);
    m_Drawing = true;
    RawDrawIndexedPrimitive(t, minIndex, numVerts, startIndex, count);
    m_Drawing = false;
    m_Device->SetTexture(1, tex);
    m_Device->SetRenderState(D3DRS_TEXTUREFACTOR, tf);
    m_Device->SetTransform(D3DTS_TEXTURE1, &keepTm);
    for (size_t i = sizeof(set) / sizeof(set[0]); i-- > 0;)
        m_Device->SetTextureStageState(set[i].stage, set[i].type, keep[i]);
    return true;
}
