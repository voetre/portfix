// portfix core: the fixes, shared by the Ashita and Windower builds.
// The host calls the On* functions from its Direct3D8 draw hooks and draws through Raw*, which must reach the
// device without coming back into the fixes as a game draw.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d8.h>

class Fixes
{
public:
    IDirect3DDevice8* m_Device = nullptr;
    bool m_Water               = true;
    bool m_Halo                = true;

    virtual ~Fixes() = default;
    virtual void RawDrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count)                  = 0;
    virtual void RawDrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride) = 0;

    // True: the draw was handled here and the original must be skipped.
    bool OnDrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count);
    void OnDrawIndexedPrimitive(void);
    bool OnDrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride);
    void OnPresent(void);
    void SetWater(bool on);
    void Release(void);

private:
    bool m_Drawing = false; // a Raw* draw is in progress

    bool m_WaterHeld = false; // a ripple layer waits to be drawn after the tint
    D3DPRIMITIVETYPE m_WaterType = D3DPT_TRIANGLELIST;
    UINT m_WaterStart = 0, m_WaterCount = 0;
    DWORD m_WaterBlock = 0, m_WaterKeep = 0; // state blocks: the ripple draw's state, the state put back after

    IDirect3DBaseTexture8* m_HaloSrc = nullptr; // this frame's silhouette and its 256x256 copy's surface (identity only, not held)
    IDirect3DSurface8* m_HaloLow     = nullptr;

    bool WaterSurface(void) const;
    bool WaterHold(D3DPRIMITIVETYPE t, UINT start, UINT count);
    void WaterFlush(void);
    bool HaloDraw(D3DPRIMITIVETYPE t, UINT count, const void* data);
};
