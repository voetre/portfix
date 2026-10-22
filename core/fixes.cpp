#include "fixes.h"

#include <cstring>
#include <utility>

bool Fixes::OnDrawPrimitive(const D3DPRIMITIVETYPE t, const UINT start, const UINT count)
{
    if (m_Drawing || m_Device == nullptr)
        return false;
    if (m_Disint)
        DisintBeforeDraw();
    if (m_Water && WaterSurface()) // the ripple layer waits for the tint layer
    {
        WaterFlush();
        return WaterHold(t, start, count);
    }
    if (m_WaterHeld) // the tint: draw it, then the held ripples on top
    {
        m_Drawing = true;
        RawDrawPrimitive(t, start, count);
        m_Drawing = false;
        WaterFlush();
        return true;
    }
    return false;
}

bool Fixes::OnDrawIndexedPrimitive(const D3DPRIMITIVETYPE t, const UINT minIndex, const UINT numVerts, const UINT startIndex, const UINT count)
{
    if (m_Drawing || m_Device == nullptr)
        return false;
    if (m_WaterHeld)
        WaterFlush();
    if (!m_Disint)
        return false;
    DisintBeforeDraw();
    return DisintDraw(t, minIndex, numVerts, startIndex, count);
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
    return error;
}

void Fixes::Release(void)
{
    SetDisint(false);
    WaterRelease();
}

void Fixes::WaterRelease(void)
{
    m_WaterHeld = false;
    for (DWORD* b : {&m_WaterBlock, &m_WaterKeep})
        if (*b != 0 && m_Device != nullptr)
            m_Device->DeleteStateBlock(*b), *b = 0;
}

// Layered water (Bastok Markets' canals and others) is three flat planes about 0.03 apart: a black layer that
// darkens with depth, the scrolling ripple layer added on top (writes depth), then a dark tint on the black layer's
// geometry, which sits just under the ripples. With coarse (16-bit, PS2-era) depth the planes had the same depth
// and the tint passed LESSEQUAL; with the PC's 24-bit (or DXVK's float) depth it fails up close and the water is
// black, on any GPU.
// The fix holds the ripple draw back (its whole state in a state block) and draws it after the tint, with no depth
// write: tint, then ripples added on top. Drawing the ripples in place with no depth write showed flat tint only.
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

bool Fixes::WaterHold(const D3DPRIMITIVETYPE t, const UINT start, const UINT count)
{
    if (m_WaterBlock == 0 ? FAILED(m_Device->CreateStateBlock(D3DSBT_ALL, &m_WaterBlock)) : FAILED(m_Device->CaptureStateBlock(m_WaterBlock)))
    {
        m_WaterBlock = 0;
        return false; // the game draws it as it is
    }
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
    if (m_WaterKeep == 0 ? FAILED(m_Device->CreateStateBlock(D3DSBT_ALL, &m_WaterKeep)) : FAILED(m_Device->CaptureStateBlock(m_WaterKeep)))
    {
        m_WaterKeep = 0;
        return;
    }
    m_Device->ApplyStateBlock(m_WaterBlock);
    m_Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_Drawing = true;
    RawDrawPrimitive(m_WaterType, m_WaterStart, m_WaterCount);
    m_Drawing = false;
    m_Device->ApplyStateBlock(m_WaterKeep);
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
    m_Drawing = true;
    RawDrawPrimitiveUP(t, count, v, 28);
    m_Drawing = false;
    m_Device->SetTexture(0, tex);
    return true;
}
