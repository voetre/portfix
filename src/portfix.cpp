/**
 * portfix - Ashita v4 plugin
 *
 * Fixes for graphics the PC port of FINAL FANTASY XI draws wrong. Both effects were made for the PS2 renderer
 * and carried over to the PC unchanged, so they break on PC depth buffers and resolutions. Each fix works on the
 * game's own draw calls: nothing is drawn that the game doesn't draw itself.
 *
 * Commands:
 *   /portfix                     - show what is on
 *   /portfix water [on|off]      - black water fix (Bastok's canals and other layered water)
 *   /portfix glow [on|off|<n>]   - summon halo fix (Carbuncle's outline and others); a number sets the ghost
 *                                  spacing scale (0 = render width / 640)
 *
 * See docs/NOTES.md for how each bug was found.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "Ashita.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

class PortFix final : public IPlugin
{
    IAshitaCore* m_AshitaCore  = nullptr;
    ILogManager* m_LogManager  = nullptr;
    IDirect3DDevice8* m_Device = nullptr;
    bool m_Drawing             = false; // our own draws come back through the hooks

    bool m_WaterFix  = true;
    bool m_WaterHeld = false; // a ripple layer waits to be drawn after the tint
    D3DPRIMITIVETYPE m_WaterType = D3DPT_TRIANGLELIST;
    UINT m_WaterStart = 0, m_WaterCount = 0;
    DWORD m_WaterBlock = 0, m_WaterKeep = 0; // state blocks: the ripple draw's state, the state put back after

    bool m_GlowFix     = true;
    float m_GlowSpread = 0.0f; // ghost offset scale; 0 = render width / 640
    IDirect3DBaseTexture8* m_GlowSrc = nullptr; // this frame's halo silhouette and its 256x256 copy's surface (identity only, not held)
    IDirect3DSurface8* m_GlowLow     = nullptr;

    bool WaterSurface(void) const;
    bool WaterHold(D3DPRIMITIVETYPE t, UINT start, UINT count);
    void WaterFlush(void);
    void WaterRelease(void);
    bool GlowDraw(D3DPRIMITIVETYPE t, UINT count, const void* data);
    void Print(const std::string& msg) const;
    static const char* OnOff(const bool b) { return b ? "on" : "off"; }

public:
    const char* GetName(void) const override { return "portfix"; }
    const char* GetAuthor(void) const override { return "voetre"; }
    const char* GetDescription(void) const override { return "Fixes for graphics the PC port draws wrong: black water, summon halos."; }
    const char* GetLink(void) const override { return ""; }
    double GetVersion(void) const override { return 1.0; }
    // Early, so the fixes see the game's draws before other plugins act on them.
    int32_t GetPriority(void) const override { return -10; }
    uint32_t GetFlags(void) const override { return static_cast<uint32_t>(Ashita::PluginFlags::UseCommands | Ashita::PluginFlags::UseDirect3D); }

    bool Initialize(IAshitaCore* core, ILogManager* logger, const uint32_t id) override
    {
        m_AshitaCore = core;
        m_LogManager = logger;
        return true;
    }
    void Release(void) override { WaterRelease(); }
    bool Direct3DInitialize(IDirect3DDevice8* device) override
    {
        m_Device = device;
        return true;
    }
    void Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*) override
    {
        m_WaterHeld = false; // a ripple draw with nothing after it in the frame is dropped
        m_GlowSrc   = nullptr;
        m_GlowLow   = nullptr;
    }
    bool Direct3DDrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count) override
    {
        if (m_Drawing)
            return false;
        if (m_WaterFix && WaterSurface()) // the water's ripple layer waits for the tint layer
        {
            WaterFlush();
            return WaterHold(t, start, count);
        }
        if (m_WaterHeld) // the tint: draw it, then the held ripples on top
        {
            m_Drawing = true;
            m_Device->DrawPrimitive(t, start, count);
            m_Drawing = false;
            WaterFlush();
            return true;
        }
        return false;
    }
    bool Direct3DDrawIndexedPrimitive(D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT) override
    {
        if (m_WaterHeld && !m_Drawing)
            WaterFlush();
        return false;
    }
    bool Direct3DDrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, CONST void* data, UINT stride) override
    {
        if (m_Drawing)
            return false;
        if (m_WaterHeld)
            WaterFlush();
        return m_GlowFix && stride == 28 && GlowDraw(t, count, data);
    }
    bool HandleCommand(int32_t mode, const char* command, bool injected) override;
};

// Layered water (Bastok Markets' canals and others) is three flat planes about 0.03 apart: a black layer that
// darkens with depth, the scrolling ripple layer added on top (writes depth), then a dark tint on the black layer's
// geometry, which sits just under the ripples. With coarse (16-bit, PS2-era) depth the planes had the same depth
// and the tint passed LESSEQUAL; with the PC's 24-bit (or DXVK's float) depth it fails up close and the water is
// black, on any GPU.
// The fix holds the ripple draw back (its whole state in a state block) and draws it after the tint, with no depth
// write: tint, then ripples added on top. Drawing the ripples in place with no depth write showed flat tint only.
// The ripple layer: FVF XYZ|NORMAL|DIFFUSE|TEX1, unlit, additive (SRCALPHA, ONE), writes depth, scrolling texture.
bool PortFix::WaterSurface(void) const
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

bool PortFix::WaterHold(const D3DPRIMITIVETYPE t, const UINT start, const UINT count)
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

void PortFix::WaterFlush(void)
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
    m_Device->DrawPrimitive(m_WaterType, m_WaterStart, m_WaterCount);
    m_Drawing = false;
    m_Device->ApplyStateBlock(m_WaterKeep);
}

void PortFix::WaterRelease(void)
{
    m_WaterHeld = false;
    for (DWORD* b : {&m_WaterBlock, &m_WaterKeep})
        if (*b != 0 && m_Device != nullptr)
            m_Device->DeleteStateBlock(*b), *b = 0;
}

// Outline halos (Carbuncle's blue aura, other summons): the model is drawn into a 4096x4096 silhouette texture,
// copied down to 256x256, then the copy is drawn back over the scene as full-screen quads: three alpha-blended
// tinted ghosts nudged ~3 px in a pattern that turns each frame, one opaque alpha-tested copy, then the silhouette
// itself on top. The 256 copy is 16 scene pixels a texel, so the opaque copy sticks out past the model as stair
// steps, and ~3 px nudges (PS2 frame pixels, kept as-is) don't separate the ghosts at 4096.
// The fix draws those quads from the full-size silhouette and scales the nudges to the render width.
// Quads: XYZRHW|DIFFUSE|TEX1 (stride 28), a 4-vertex strip from (dx, dy) to (w + dx, h + dy).
bool PortFix::GlowDraw(const D3DPRIMITIVETYPE t, const UINT count, const void* data)
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
        m_GlowSrc = tex;
        m_GlowLow = rt;
        rt->Release();
        return false;
    }
    rt->Release();
    if (m_GlowSrc == nullptr || tex->GetType() != D3DRTYPE_TEXTURE)
        return false;
    IDirect3DSurface8* s0 = nullptr;
    if (FAILED(static_cast<IDirect3DTexture8*>(tex)->GetSurfaceLevel(0, &s0)) || s0 == nullptr)
        return false;
    s0->Release();
    if (s0 != m_GlowLow)
        return false;
    BYTE v[4 * 28];
    std::memcpy(v, data, sizeof(v));
    float x0, y0;
    std::memcpy(&x0, v, 4);
    std::memcpy(&y0, v + 4, 4);
    const float k = m_GlowSpread > 0.0f ? m_GlowSpread : static_cast<float>(d.Width) / 640.0f;
    for (int i = 0; i < 4; ++i)
    {
        float xy[2];
        std::memcpy(xy, v + i * 28, 8);
        xy[0] += x0 * (k - 1.0f);
        xy[1] += y0 * (k - 1.0f);
        std::memcpy(v + i * 28, xy, 8);
    }
    m_Device->SetTexture(0, m_GlowSrc);
    m_Drawing = true;
    m_Device->DrawPrimitiveUP(t, count, v, 28);
    m_Drawing = false;
    m_Device->SetTexture(0, tex);
    return true;
}

bool PortFix::HandleCommand(int32_t mode, const char* command, bool injected)
{
    std::vector<std::string> args;
    if (Ashita::Commands::GetCommandArgs(command, &args) == 0 || !Ashita::Commands::CommandCheck(args[0], {"/portfix"}))
        return false;
    const std::string sub = args.size() > 1 ? args[1] : "";
    if (sub == "water")
        m_WaterFix = args.size() > 2 ? args[2] == "on" : !m_WaterFix;
    else if (sub == "glow")
    {
        if (args.size() > 2 && (std::isdigit(static_cast<unsigned char>(args[2][0])) || args[2][0] == '.'))
            m_GlowSpread = static_cast<float>(std::atof(args[2].c_str())), m_GlowFix = true;
        else
            m_GlowFix = args.size() > 2 ? args[2] == "on" : !m_GlowFix;
    }
    else if (!sub.empty())
    {
        Print("Usage: /portfix [water [on|off]] [glow [on|off|<spread>]]");
        return true;
    }
    if (!m_WaterFix)
        WaterRelease();
    Print(std::string("Black water fix: ") + OnOff(m_WaterFix) + ", summon halo fix: " + OnOff(m_GlowFix) +
          (m_GlowSpread > 0.0f ? " (spread x" + std::to_string(m_GlowSpread) + ")" : ""));
    return true;
}

void PortFix::Print(const std::string& msg) const
{
    m_AshitaCore->GetChatManager()->Write(1, false, (Ashita::Chat::Header("portfix") + Ashita::Chat::Message(msg)).c_str());
    m_LogManager->Log(static_cast<uint32_t>(Ashita::LogLevel::Info), "portfix", msg.c_str());
}

extern "C" double __stdcall expGetInterfaceVersion(void)
{
    return ASHITA_INTERFACE_VERSION;
}

extern "C" IPlugin* __stdcall expCreatePlugin(const char* args)
{
    return new PortFix();
}

extern "C" void __stdcall expDestroyPlugin(void* instance)
{
    delete static_cast<PortFix*>(instance);
}
