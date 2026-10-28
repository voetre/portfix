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
    bool m_Shine               = true;
    bool m_Disint              = false; // set through SetDisint: it patches the game's code
    bool m_Split               = false; // set through SetSplit: shine batches the PC files merged (patches the skinning call)

    virtual ~Fixes() = default;
    virtual void RawDrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count)                  = 0;
    virtual void RawDrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride) = 0;
    virtual void RawDrawIndexedPrimitive(D3DPRIMITIVETYPE t, UINT minIndex, UINT numVerts, UINT startIndex, UINT count) = 0;

    // True: the draw was handled here and the original must be skipped.
    bool OnDrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count);
    bool OnDrawIndexedPrimitive(D3DPRIMITIVETYPE t, UINT minIndex, UINT numVerts, UINT startIndex, UINT count);
    bool OnDrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride);
    void OnPresent(void);
    void SetWater(bool on);
    // nullptr when it's in the state asked for, or the reason it couldn't be turned on.
    const char* SetDisint(bool on);
    const char* SetSplit(bool on);
    void Release(void);
    // The setup, for a bug report: with /portfix debug on (debuglog.h).
    void DebugReport(const char* version) const;

private:
    bool m_Drawing = false; // a Raw* draw is in progress

    bool m_WaterHeld = false; // a ripple layer waits to be drawn after the tint
    D3DPRIMITIVETYPE m_WaterType = D3DPT_TRIANGLELIST;
    UINT m_WaterStart = 0, m_WaterCount = 0;
    // Device state portfix saves and puts back around its own draws (the water's held ripple layer, disintegrate's
    // pieces). Kept by hand, not in D3D8 state blocks: wrappers that implement D3D8 themselves (dgVoodoo) may not
    // capture and apply all of a block; with dgVoodoo the ripples came out a flat opaque sheet (GitHub issue #1).
    struct DrawState
    {
        DWORD rs[48]{};
        DWORD tss[2][20]{};
        D3DMATRIX world{}, view{}, proj{}, tex0{}, tex1{};
        IDirect3DBaseTexture8* texture[4]{};
        IDirect3DVertexBuffer8* stream = nullptr;
        UINT stride  = 0;
        DWORD shader = 0, pixelShader = 0;
        bool held    = false;
        void Capture(IDirect3DDevice8* d);
        void Apply(IDirect3DDevice8* d) const;
        void Clear(void);
    };
    DrawState m_WaterState, m_WaterKeep;

    IDirect3DBaseTexture8* m_HaloSrc = nullptr; // this frame's silhouette and its 256x256 copy's surface (identity only, not held)
    IDirect3DSurface8* m_HaloLow     = nullptr;

    bool WaterSurface(void) const;
    static constexpr float WaterLift = 0.05f; // yalms; Bastok Markets' water lies 0.0415 under the canal's plane
    bool WaterLayer(void) const;
    void WaterDraw(D3DPRIMITIVETYPE t, UINT start, UINT count, bool lift);
    bool WaterHold(D3DPRIMITIVETYPE t, UINT start, UINT count);
    void WaterFlush(void);
    void WaterRelease(void);
    bool HaloDraw(D3DPRIMITIVETYPE t, UINT count, const void* data);

    int m_ShineTemp = -1; // the device has D3DTA_TEMP and three stages: 1, not: 0, not asked yet: -1
    bool m_SpotOps  = false; // and MODULATE4X and cube textures (set with m_ShineTemp)
    bool ShineDraw(D3DPRIMITIVETYPE t, UINT minIndex, UINT numVerts, UINT startIndex, UINT count);
    bool ShineCaps(void);
    // The highlight cube for reflection-vector shine: rebuilt from the game's own cube (m_SpotFrom) with the PS2's
    // spread; SpotSpread is the spot texture's radius (0.5 = its edge) per sin(angle / 2) from the light.
    IDirect3DCubeTexture8* m_SpotCube    = nullptr;
    IDirect3DBaseTexture8* m_SpotFrom    = nullptr;
    static constexpr float SpotSpread    = 0.6f;
    static constexpr UINT SpotSize       = 128;
    bool SpotBuild(IDirect3DCubeTexture8* game);
    void SpotRelease(void);
    bool SpotDraw(D3DPRIMITIVETYPE t, UINT minIndex, UINT numVerts, UINT startIndex, UINT count);

    // disint.cpp
    const char* DisintInstall(void);
    void DisintRemove(void);
    bool DisintDraw(D3DPRIMITIVETYPE t, UINT minIndex, UINT numVerts, UINT startIndex, UINT count);
    void DisintBeforeDraw(void);
    void DisintDrawPieces(void);
    void DisintDrawHaze(void);
    void DisintPresent(void);
};
