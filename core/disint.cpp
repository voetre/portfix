// Disintegrate: the PS2's body break-up effect, which the PC client skips. See docs/TECHNICAL.md.
// Effect files such as 16/98 (the Shadow Lord's change of form) sweep a particle over a model. Every frame the
// particle's op 0x65 marks a sphere around it; on the PS2 each body triangle with a vertex inside a sphere leaves the
// model (the body stops drawing it) and flies off as a textured piece, with purple smoke and a heat haze at the
// break. The PC runs the same particle lists, but its per-particle interpreter sends op 0x65 to the shared
// do-nothing case.
//
// The fix points that op's jump table entry at a stub that records the sphere, then runs the game's case. The body
// is skinned on the CPU into world space (the world transform is identity), one draw per part with its own index
// range. While a capturing actor's part is drawn, every triangle with a vertex inside a sphere is taken: copied out
// (positions, UVs, texture, lit colours) as a piece, and the part is drawn from our own index buffer without it.
#include "fixes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>

#include "shinesplit.h"

namespace
{
    constexpr float Tick    = 60.0f / 1000.0f; // effect ticks per millisecond
    constexpr float PsFrame = 30.0f / 1000.0f; // PS2 frames per millisecond

    // The PS2's values (16/98's generators, and its elements read live)
    constexpr float PieceLife  = 40.0f;  // mt00's 'disg': life 0x28 ticks
    constexpr float PieceRise  = 0.012f; // rise a tick (op 02)
    constexpr float PieceDrift = 0.003f; // up to this much more or less (op 03)
    constexpr float PieceEnd   = 0.02f;  // size at the end of its life (keyframes mtx0)
    constexpr float PieceSpin[3]{0.0314f, 0.0332f, 0.0332f}; // up to this many radians a tick about x, y, z (op 0c)
    constexpr D3DCOLOR PieceColour = D3DCOLOR_ARGB(255, 49, 32, 41); // an unlit piece: the body's usual lit colour
    constexpr float HazeLife  = 30.0f;  // g000's 'dist': life 0x1e ticks
    constexpr float HazeShift = 0.07f;  // how far it shifts the picture behind, in its own units
    constexpr DWORD HoldMs    = 3000;   // an actor with no spheres for this long gets its body back
    constexpr DWORD UiFvf     = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1; // the game's 2D interface draws

    struct Sphere
    {
        const float* bones; // the actor's bone matrices = its skinning palette
        float c[3], r;
    };

    // One part draw: an index range of an index buffer, as triangles (a list or a strip unpacked).
    struct Part
    {
        std::vector<uint16_t> tri;  // 3 per triangle, the game's index values
        std::vector<uint8_t> taken; // per triangle
        uint32_t takenCount       = 0;
        IDirect3DIndexBuffer8* ib = nullptr; // ours: the triangles not taken
        bool dirty                = false;
        uint32_t kept             = 0;
    };

    struct Actor
    {
        std::map<uint64_t, Part> parts;
        DWORD lastSphere = 0;
    };

    struct Piece
    {
        float p[3][3], uv[3][2];
        float spin[3]; // radians a tick about x, y, z
        float drift;   // extra rise a tick
        IDirect3DBaseTexture8* tex;
        DWORD born;
        D3DCOLOR c[3]; // the body's lit vertex colours, as the PS2 copies them
        bool lit;      // c is set (the body draw was lit and had normals)
    };

    struct Haze
    {
        float p[3];
        DWORD born;
    };

    struct Vtx
    {
        float x, y, z;
        D3DCOLOR c;
        float u, v;
    };

    // The fixed-function lighting a body draw had (lights, material, ambient), to colour its taken triangles the
    // way the PS2 does: its pieces carry the body's lit vertex colours.
    struct LightSet
    {
        int n = 0;
        D3DLIGHT8 l[8]{};
        D3DMATERIAL8 m{};
        float amb[3]{};
    };

    // The views the bodies were drawn with this frame, one per render target (scene, shadows, reflections):
    // the pieces go to the target the interface is drawn on.
    struct View
    {
        void* target;
        float view[16], proj[16];
    };

    // Game functions. Both are __thiscall; __fastcall with an unused edx makes the same call on either compiler.
    using WorldPosFn = void(__fastcall*)(void* particle, void* edx, float* out);
    using EmitFn     = bool(__fastcall*)(void* generator, void* edx, void* parent);
    using SkinFn     = void(__fastcall*)(uintptr_t part);
    WorldPosFn s_WorldPos = nullptr; // a particle's world position
    EmitFn s_Emit         = nullptr; // a generator emits on a parent particle (creation op 0x3c); optional
    SkinFn s_Skin         = nullptr; // skin_part_b's skinning call
    int32_t* s_SkinCall   = nullptr; // that call's rel32
    void** s_Slot         = nullptr; // op 0x65's entry in the per-particle jump table
    void* s_Orig          = nullptr; // the game's case (do nothing)
    uint8_t* s_Stub       = nullptr; // our case
    float** s_Palette     = nullptr; // the skinning palette pointer skin_part_b reads
    const uint8_t* s_SkinFn = nullptr; // skin_part_b
    int s_SkinUsers         = 0;       // who needs the skinning call hooked: 1 disintegration, 2 the shine split
    bool s_SplitOn          = false;

    const float* s_Part = nullptr; // the palette of the part being skinned, when it's a capturing actor's
    // The positions go to the buffer at mesh +0x44 (locked to +0x8e) and the normals to +0x48 (+0x92); for parts
    // with flag 1 at +0x34 also positions to +0x50 (+0x96) and normals to +0x54 (+0x9a).
    void* s_SkinVb[4]{}; // the buffers s_SkinPos holds this part's positions (0, 1) and normals (2, 3) for
    std::vector<float> s_SkinPos[4];

    std::vector<Sphere> s_New;     // spheres marked since the last Present
    std::vector<Sphere> s_Spheres; // spheres in use this frame
    std::map<const float*, Actor> s_Actors; // by bone palette
    std::vector<Piece> s_Pieces;
    std::vector<Haze> s_Haze;
    IDirect3DTexture8* s_HazeTex = nullptr; // a copy of the scene target, which the haze shows shifted
    View s_Views[4];
    size_t s_ViewCount = 0;
    float s_View[16], s_Proj[16];
    bool s_Drawn = false; // pieces drawn this frame

    uint32_t s_Hits = 0, s_LastHits = 0;          // triangles taken since the last Present, and in the frame before
    float s_HitAt[4][3]{}, s_LastHitAt[4][3]{};   // four of them picked at random: their centroids
    DWORD s_SmokeAt = 0;                          // when the smoke last emitted
    // The PS2 makes one 'disg' element a drawn frame (30 fps) from all that frame's taken triangles, so they share
    // its tumble and rise.
    uint32_t s_ElemFrame = 0;
    float s_ElemSpin[3]{}, s_ElemDrift = 0.0f;

    float Rand(void) // 0..1
    {
        static uint32_t s = 0x9E3779B9u;
        s                 = s * 1664525u + 1013904223u;
        return static_cast<float>(s >> 8) / 16777216.0f;
    }

    bool Readable(const void* p, const size_t n)
    {
        return reinterpret_cast<uintptr_t>(p) >= 0x10000 && !::IsBadReadPtr(p, n);
    }

    template <class T>
    bool ReadAt(const uint32_t at, T& out)
    {
        if (!Readable(reinterpret_cast<const void*>(at), sizeof(T)))
            return false;
        std::memcpy(&out, reinterpret_cast<const void*>(at), sizeof(T));
        return true;
    }

    float Dist2(const float* a, const float* b)
    {
        const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
        return x * x + y * y + z * z;
    }

    void* TargetOf(IDirect3DDevice8* dev)
    {
        IDirect3DSurface8* rt = nullptr;
        if (FAILED(dev->GetRenderTarget(&rt)) || rt == nullptr)
            return nullptr;
        rt->Release(); // only compared, never used
        return rt;
    }

    D3DCOLOR LitColour(const LightSet& s, const float* p, const float* nIn)
    {
        float n[3]{nIn[0], nIn[1], nIn[2]};
        const float nl2 = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
        const float inv = nl2 > 1e-12f ? 1.0f / std::sqrt(nl2) : 0.0f;
        for (float& x : n)
            x *= inv;
        const float ma[3]{s.m.Ambient.r, s.m.Ambient.g, s.m.Ambient.b}, md[3]{s.m.Diffuse.r, s.m.Diffuse.g, s.m.Diffuse.b};
        float c[3]{s.m.Emissive.r + ma[0] * s.amb[0], s.m.Emissive.g + ma[1] * s.amb[1], s.m.Emissive.b + ma[2] * s.amb[2]};
        for (int i = 0; i < s.n; ++i)
        {
            const D3DLIGHT8& l = s.l[i];
            float L[3], att = 1.0f;
            if (l.Type == D3DLIGHT_DIRECTIONAL)
                L[0] = -l.Direction.x, L[1] = -l.Direction.y, L[2] = -l.Direction.z;
            else
            {
                L[0] = l.Position.x - p[0], L[1] = l.Position.y - p[1], L[2] = l.Position.z - p[2];
                const float d = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
                if (d > l.Range)
                    continue;
                const float q = l.Attenuation0 + l.Attenuation1 * d + l.Attenuation2 * d * d;
                att           = q > 1e-6f ? 1.0f / q : 1.0f;
            }
            const float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
            if (ll < 1e-12f)
                continue;
            for (float& x : L)
                x /= ll;
            if (l.Type == D3DLIGHT_SPOT)
            {
                const float dl  = std::sqrt(l.Direction.x * l.Direction.x + l.Direction.y * l.Direction.y + l.Direction.z * l.Direction.z);
                const float rho = dl > 1e-12f ? -(L[0] * l.Direction.x + L[1] * l.Direction.y + L[2] * l.Direction.z) / dl : 1.0f;
                const float cp = std::cos(l.Phi * 0.5f), ct = std::cos(l.Theta * 0.5f);
                if (rho <= cp)
                    continue;
                if (rho < ct && ct > cp)
                    att *= std::pow((rho - cp) / (ct - cp), l.Falloff);
            }
            const float nd = (std::max)(0.0f, n[0] * L[0] + n[1] * L[1] + n[2] * L[2]);
            const float la[3]{l.Ambient.r, l.Ambient.g, l.Ambient.b}, ld[3]{l.Diffuse.r, l.Diffuse.g, l.Diffuse.b};
            for (int j = 0; j < 3; ++j)
                c[j] += att * (ma[j] * la[j] + md[j] * ld[j] * nd);
        }
        const auto b = [](const float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
        return D3DCOLOR_ARGB(255, b(c[0]), b(c[1]), b(c[2]));
    }

    const float* BonesOf(const uint32_t actor)
    {
        uint32_t holder = 0, mats = 0;
        if (!ReadAt(actor + 0x6B8, holder) || !ReadAt(holder + 0x14, mats) || mats < 0x10000)
            return nullptr;
        return reinterpret_cast<const float*>(mats);
    }

    // On frames that took triangles, the PS2 runs g000's creation list once a drawn frame (30 fps), at one of the
    // taken triangles picked at random: a 'dist' element (the haze) whose creation list starts mt01, the purple
    // smoke, at it (op 0x3c). The PC has both elements but its op 0x65 never runs g000. This does op 0x3c's part:
    // emit mt01 on a stand-in parent at the triangle (a copy of this particle moved there; the emit only reads its
    // parent). Record +8 is g000's handle; op 0x3c's record +4 in g000's creation list is mt01's.
    void Smoke(const uint32_t particle, const uint32_t rec)
    {
        uint32_t handle = 0, smoke = 0, list = 0, op = 0;
        if (s_Emit != nullptr && ReadAt(rec + 8, handle) && ReadAt(handle, smoke) && smoke != 0 && ReadAt(smoke + 0xe4, list) && list != 0)
        {
            smoke = 0;
            for (int i = 0; i < 64 && ReadAt(list, op) && (op & 0xff) != 0 && (op >> 8 & 0x1f) != 0; ++i, list += 4 * (op >> 8 & 0x1f))
                if ((op & 0xff) == 0x3c && ReadAt(list + 4, handle) && handle != 0 && ReadAt(handle, smoke))
                    break;
        }
        else
            smoke = 0;
        static uint8_t parent[0x200];
        // As many as 30 Hz frames went by (one after a pause, up to four).
        const DWORD now      = ::GetTickCount();
        const uint32_t hits  = (std::min)(s_LastHits, 4u);
        const DWORD gap      = now - s_SmokeAt;
        const uint32_t ticks = (std::min)(gap > 150 ? 1u : static_cast<uint32_t>(gap * PsFrame + 0.5f), hits);
        for (uint32_t e = 0; e < ticks && s_Haze.size() < 512; ++e)
            s_Haze.push_back({{s_LastHitAt[e][0], s_LastHitAt[e][1], s_LastHitAt[e][2]}, now});
        if (ticks > 0)
            s_LastHits = 0, s_SmokeAt = gap > 150 ? now : s_SmokeAt + static_cast<DWORD>(ticks / PsFrame + 0.5f);
        for (uint32_t e = 0; e < ticks && smoke != 0 && Readable(reinterpret_cast<const void*>(smoke), 0xe8) && Readable(reinterpret_cast<const void*>(particle), sizeof(parent)); ++e)
        {
            const float* at = s_LastHitAt[e];
            // The parent's position (+0x54) is in its frame: find the one that puts it at the triangle. The world
            // position is affine in it, so three unit steps give the matrix.
            std::memcpy(parent, reinterpret_cast<const void*>(particle), sizeof(parent));
            float* local = reinterpret_cast<float*>(parent + 0x54);
            float w0[3], m[3][3], l0[3];
            std::memcpy(l0, local, sizeof(l0));
            s_WorldPos(parent, nullptr, w0);
            for (int c = 0; c < 3; ++c)
            {
                float w[3];
                local[c] += 1.0f;
                s_WorldPos(parent, nullptr, w);
                local[c] -= 1.0f;
                for (int r = 0; r < 3; ++r)
                    m[r][c] = w[r] - w0[r];
            }
            const auto det3 = [](const float (&k)[3][3]) {
                return k[0][0] * (k[1][1] * k[2][2] - k[1][2] * k[2][1]) - k[0][1] * (k[1][0] * k[2][2] - k[1][2] * k[2][0]) + k[0][2] * (k[1][0] * k[2][1] - k[1][1] * k[2][0]);
            };
            const float det = det3(m);
            if (std::fabs(det) <= 1e-9f)
                continue;
            const float d[3] = {at[0] - w0[0], at[1] - w0[1], at[2] - w0[2]};
            for (int c = 0; c < 3; ++c) // Cramer's rule
            {
                float k[3][3];
                std::memcpy(k, m, sizeof(k));
                for (int r = 0; r < 3; ++r)
                    k[r][c] = d[r];
                local[c] = l0[c] + det3(k) / det;
            }
            *reinterpret_cast<uint32_t*>(smoke + 0xcc) = 0; // the generator's countdown: emit now
            s_Emit(reinterpret_cast<void*>(smoke), nullptr, parent);
        }
    }

    // Op 0x65 of a particle. Record: 65 07 00 00 | generator name (resolved to a handle at +8) | radius at +0xc
    // (animated by the particle's op 0x6a). The generator's +0x3c is the actor it plays on.
    void __cdecl OnRegion(const uint32_t particle, const uint32_t rec, const uint32_t gen)
    {
        if (s_WorldPos == nullptr)
            return;
        uint32_t actor = 0;
        float r        = 0.0f;
        if (!ReadAt(gen + 0x3c, actor) || !ReadAt(rec + 0xc, r))
            return;
        const float* bones = BonesOf(actor);
        if (bones == nullptr || !(r > 0.0f) || r > 1000.0f)
            return;
        Sphere sp{bones, {}, r};
        s_WorldPos(reinterpret_cast<void*>(particle), nullptr, sp.c);
        if (s_New.size() < 256)
            s_New.push_back(sp);
        Smoke(particle, rec);
    }

    // skin_part_b's call to its skinning (__fastcall, the part in ecx). The skinning writes the positions to the
    // vertex buffer the game locked (D3DLOCK_DISCARD); reading that back for the capture stalls (write-combined
    // memory: up to 60 ms a frame). For a capturing actor's part it writes to s_SkinPos instead, copied to the
    // buffer after. The palette it skins with says whose part it is.
    void SplitPart(uintptr_t part);
    void __fastcall SkinHook(const uintptr_t part)
    {
        if (s_SplitOn)
            SplitPart(part);
        for (void*& vb : s_SkinVb)
            vb = nullptr;
        const float* pal = (s_SkinUsers & 1) != 0 && s_Palette != nullptr ? *s_Palette : nullptr;
        s_Part           = pal != nullptr && s_Actors.count(pal) != 0 ? pal : nullptr;
        const auto mesh  = s_Part != nullptr ? *reinterpret_cast<const uintptr_t*>(part + 0x5E) : 0;
        const uint32_t n = mesh != 0 ? *reinterpret_cast<const uint32_t*>(mesh + 0x58) : 0;
        if (mesh == 0 || n == 0 || n > 0x10000)
            return s_Skin(part);
        const bool second = (*reinterpret_cast<const uint8_t*>(part + 0x34) & 1) != 0 && *reinterpret_cast<void* const*>(mesh + 0x50) != nullptr && *reinterpret_cast<void* const*>(mesh + 0x54) != nullptr;
        static constexpr int buf[4] = {0x44, 0x50, 0x48, 0x54}, ptr[4] = {0x8E, 0x96, 0x92, 0x9A}; // buffer, and where its lock points
        float** out[4] = {};
        for (int k = 0; k < 4; ++k)
            if (second || (k & 1) == 0)
                out[k] = reinterpret_cast<float**>(mesh + ptr[k]);
        float* real[4] = {};
        for (int k = 0; k < 4; ++k)
            if (out[k] != nullptr && (real[k] = *out[k]) != nullptr)
            {
                s_SkinPos[k].resize(static_cast<size_t>(n) * 3);
                *out[k] = s_SkinPos[k].data();
            }
        s_Skin(part);
        for (int k = 0; k < 4; ++k)
            if (real[k] != nullptr)
            {
                *out[k] = real[k];
                std::memcpy(real[k], s_SkinPos[k].data(), static_cast<size_t>(n) * 12);
                s_SkinVb[k] = *reinterpret_cast<void* const*>(mesh + buf[k]);
            }
    }

    const IMAGE_NT_HEADERS* GameHeaders(const uint8_t* base)
    {
        return reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    }

    // Whether code lies in FFXiMain (not a hook another plugin put in its place).
    bool InModule(const void* at)
    {
        const auto base = reinterpret_cast<const uint8_t*>(::GetModuleHandleA("FFXiMain.dll"));
        const auto p    = static_cast<const uint8_t*>(at);
        return base != nullptr && p >= base && p < base + GameHeaders(base)->OptionalHeader.SizeOfImage;
    }

    // First match of a byte pattern ('?' = any byte) in FFXiMain's code. With an entry, the match is that far
    // into a function whose first bytes are the entry or a 5-byte jmp another plugin hooked it with, and the
    // function's start is returned.
    const uint8_t* FindBytes(const char* pattern, const size_t length, const char* entry = nullptr, const size_t lead = 0)
    {
        const auto base = reinterpret_cast<const uint8_t*>(::GetModuleHandleA("FFXiMain.dll"));
        if (base == nullptr)
            return nullptr;
        const auto nt      = GameHeaders(base);
        const auto section = IMAGE_FIRST_SECTION(nt);
        for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s)
        {
            if ((section[s].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                continue;
            const uint8_t* p   = base + section[s].VirtualAddress + lead;
            const uint8_t* end = base + section[s].VirtualAddress + section[s].Misc.VirtualSize - length - 4;
            for (; p < end; ++p)
            {
                size_t n = 0;
                while (n < length && (pattern[n] == '?' || p[n] == static_cast<uint8_t>(pattern[n])))
                    ++n;
                if (n != length)
                    continue;
                const uint8_t* fn = p - lead;
                if (lead == 0 || std::memcmp(fn, entry, lead) == 0 || (lead == 5 && fn[0] == 0xE9))
                    return fn;
            }
        }
        return nullptr;
    }

    bool Poke(void* at, const void* data, const size_t n)
    {
        DWORD old = 0;
        if (!::VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old))
            return false;
        std::memcpy(at, data, n);
        ::VirtualProtect(at, n, old, &old);
        ::FlushInstructionCache(::GetCurrentProcess(), at, n);
        return true;
    }

    // Op 0x65's case, written as machine code so it builds the same with either compiler. In the interpreter EBX is
    // the particle, EBP the op record and the generator is at [esp+48h]; after pushad and pushfd that's [esp+6Ch].
    //   pushad / pushfd / mov eax,[esp+6Ch] / push eax / push ebp / push ebx / mov eax,OnRegion / call eax /
    //   add esp,0Ch / popfd / popad / jmp <the game's case>
    uint8_t* MakeStub(void* orig)
    {
        auto p = static_cast<uint8_t*>(::VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (p == nullptr)
            return nullptr;
        static const uint8_t head[] = {0x60, 0x9C, 0x8B, 0x44, 0x24, 0x6C, 0x50, 0x55, 0x53, 0xB8};
        static const uint8_t tail[] = {0xFF, 0xD0, 0x83, 0xC4, 0x0C, 0x9D, 0x61, 0xE9};
        uint8_t* o = p;
        std::memcpy(o, head, sizeof(head)), o += sizeof(head);
        const auto fn = reinterpret_cast<uint32_t>(&OnRegion);
        std::memcpy(o, &fn, 4), o += 4;
        std::memcpy(o, tail, sizeof(tail)), o += sizeof(tail);
        const auto rel = static_cast<int32_t>(static_cast<uint8_t*>(orig) - (o + 4));
        std::memcpy(o, &rel, 4);
        ::FlushInstructionCache(::GetCurrentProcess(), p, 32);
        return p;
    }

    void ReleaseActor(Actor& a)
    {
        for (auto& p : a.parts)
            if (p.second.ib != nullptr)
                p.second.ib->Release();
        a.parts.clear();
    }
}

namespace
{
    // skin_part_b (sub esp,50h / push ebx,ebp / push esi,edi / mov ebx,ecx / push 0 / mov [esp+20h],ebx / call).
    // Its first five bytes may be another plugin's jmp hook, so match from push esi and accept either the original
    // entry or a jmp there.
    const char* SkinFind(void)
    {
        if (s_SkinFn != nullptr)
            return nullptr;
        static const char skin[]  = "\x56\x57\x8B\xD9\x6A\x00\x89\x5C\x24\x20\xE8";
        static const char entry[] = "\x83\xEC\x50\x53\x55";
        s_SkinFn                  = FindBytes(skin, sizeof(skin) - 1, entry, sizeof(entry) - 1);
        return s_SkinFn != nullptr ? nullptr : "the skin function wasn't found";
    }

    // skin_part_b's call to its skinning (mov ecx,ebx / call <skinning>, 0x15C0 past its start) goes through
    // SkinHook while anyone needs it: disintegration (1) or the shine split (2).
    const char* SkinUse(const int who)
    {
        if (s_SkinCall == nullptr)
        {
            const char* why = SkinFind();
            if (why != nullptr)
                return why;
            int32_t* call = nullptr;
            for (const uint8_t* c = s_SkinFn; c < s_SkinFn + 0x400 && call == nullptr; ++c)
                if (c[0] == 0x8B && c[1] == 0xCB && c[2] == 0xE8 && c + 7 + *reinterpret_cast<const int32_t*>(c + 3) == s_SkinFn + 0x15C0)
                    call = reinterpret_cast<int32_t*>(const_cast<uint8_t*>(c + 3));
            if (call == nullptr)
                return "the skinning call wasn't found";
            s_Skin         = reinterpret_cast<SkinFn>(reinterpret_cast<uint8_t*>(call + 1) + *call);
            const auto rel = static_cast<int32_t>(reinterpret_cast<const uint8_t*>(&SkinHook) - reinterpret_cast<const uint8_t*>(call + 1));
            if (!Poke(call, &rel, 4))
                return "couldn't patch the skinning call";
            s_SkinCall = call;
        }
        s_SkinUsers |= who;
        return nullptr;
    }

    void SkinUnuse(const int who)
    {
        s_SkinUsers &= ~who;
        if (s_SkinUsers != 0 || s_SkinCall == nullptr)
            return;
        const auto rel = static_cast<int32_t>(reinterpret_cast<const uint8_t*>(s_Skin) - reinterpret_cast<const uint8_t*>(s_SkinCall + 1));
        Poke(s_SkinCall, &rel, 4);
        s_SkinCall = nullptr;
    }

    // Shine split. Some PC model files lost or misplaced batch boundaries in conversion: the PS2 draws a mesh as one
    // draw per material, but the PC file put a later batch's triangles into the draw before it and left that batch's
    // material at the end of the stream with nothing to draw (M0 T D(n) M1, against the right M0 T D(a) M1 T D(b)),
    // or filed whole draws under the wrong material. The PS2's batches are in ShineSplits (shinesplit.h, measured on
    // the PS2), keyed by a hash of the texture name, the counts, the PC grouping and the triangles: (material,
    // triangles) in the stream's triangle order. A part whose stream matches gets a rebuilt copy, one draw per
    // segment, with the original triangles in their order; the part points at the copy (+0x36) from then on.
    //
    // The game writes into a stream's records once it's loaded (the texture record caches its texture), and it frees
    // a model's streams and loads them again (often at the same address) when gear changes or on zoning. So a copy
    // belongs to one original stream at one time, and every time a part is attached its records are copied again
    // from that original as it is now: a copy built for an earlier load would point at a freed texture.
    struct SplitCopy
    {
        uint8_t* buf = nullptr;     // the rebuilt stream (never freed: a part may still point here after portfix is gone)
        size_t size  = 0;
        uint32_t key = 0;           // the ShineSplits key it was built for
        const uint8_t* from = nullptr; // the original stream it currently stands in for
    };
    std::vector<SplitCopy> s_SplitCopies;
    std::map<uintptr_t, const uint8_t*> s_SplitSeen;        // part -> the stream it had when it didn't qualify
    std::map<uintptr_t, std::pair<const uint8_t*, uint8_t*>> s_SplitParts; // part -> (original, copy), for SplitUndo

    uint32_t Fnv(const uint8_t* p, const size_t n, uint32_t h = 0x811c9dc5)
    {
        for (size_t i = 0; i < n; ++i)
            h = (h ^ p[i]) * 0x01000193;
        return h;
    }

    // What a stream is: its records and triangles, and the ShineSplits entry it matches (nullptr if none).
    struct SplitInfo
    {
        std::vector<const uint8_t*> mats;
        const uint8_t* tex = nullptr;
        std::vector<uint8_t> tris;
        const ShineSplit* hit = nullptr;
        uint32_t key = 0;
    };

    bool SplitParse(const uint8_t* s, SplitInfo& in)
    {
        // walk it: materials (op 0x80n1.., 0x2e bytes), one texture (0x80n0, 0x12), triangle lists (0x54) only
        std::vector<std::pair<uint16_t, uint16_t>> groups; // (material, triangles), consecutive draws merged
        size_t p = 0;
        for (;;)
        {
            if (!Readable(s + p, 4))
                return false;
            uint16_t op, n;
            std::memcpy(&op, s + p, 2), std::memcpy(&n, s + p + 2, 2);
            if (op == 0xffff)
                break;
            if (op & 0x8000)
            {
                const int k = op >> 4 & 0xf;
                if (k == 0)
                {
                    if (in.tex != nullptr && std::memcmp(in.tex + 6, s + p + 6, 12) != 0)
                        return false; // several textures: not this case
                    in.tex = s + p, p += 0x12;
                }
                else if (k == 1)
                    in.mats.push_back(s + p), p += 0x2e;
                else
                    return false;
            }
            else if (op == 0x54)
            {
                if (in.mats.empty() || !Readable(s + p, 4 + n * 30u))
                    return false;
                const auto mi = static_cast<uint16_t>(in.mats.size() - 1);
                if (!groups.empty() && groups.back().first == mi)
                    groups.back().second = static_cast<uint16_t>(groups.back().second + n);
                else
                    groups.emplace_back(mi, n);
                in.tris.insert(in.tris.end(), s + p + 4, s + p + 4 + n * 30u), p += 4 + n * 30u;
            }
            else
                return false;
            if (p > 0x40000)
                return false;
        }
        if (in.tex == nullptr || in.mats.size() < 2 || groups.empty())
            return false;
        const uint16_t total = static_cast<uint16_t>(in.tris.size() / 30), nm = static_cast<uint16_t>(in.mats.size());
        uint8_t name[12];
        std::memcpy(name, in.tex + 6, 12);
        size_t len = 12;
        while (len > 0 && name[len - 1] == ' ')
            --len;
        size_t lead = 0;
        while (lead < len && name[lead] == ' ')
            ++lead;
        uint32_t key = Fnv(name + lead, len - lead);
        uint8_t word[4];
        std::memcpy(word, &total, 2), std::memcpy(word + 2, &nm, 2);
        key = Fnv(word, 4, key);
        for (const auto& g : groups)
            std::memcpy(word, &g.first, 2), std::memcpy(word + 2, &g.second, 2), key = Fnv(word, 4, key);
        key = Fnv(in.tris.data(), in.tris.size(), key);
        for (const auto& e : ShineSplits)
            if (e.key == key && e.n > 0)
                in.hit = &e;
        if (in.hit == nullptr)
            return false;
        size_t sum = 0;
        for (int i = 0; i < in.hit->n; ++i)
        {
            if (in.hit->mat[i] >= nm)
                return false;
            sum += in.hit->tris[i];
        }
        in.key = key;
        return sum == total;
    }

    // Build the copy's bytes into c from the stream as it is now (into its old buffer if the size fits).
    bool SplitBuild(const SplitInfo& in, SplitCopy& c)
    {
        // per segment, its material's record, the texture record, and its triangles in draws of up to 128
        std::vector<uint8_t> out;
        size_t at = 0;
        for (int i = 0; i < in.hit->n; ++i)
        {
            const uint8_t* m = in.mats[in.hit->mat[i]];
            out.insert(out.end(), m, m + 0x2e);
            out.insert(out.end(), in.tex, in.tex + 0x12);
            for (size_t left = in.hit->tris[i]; left > 0;)
            {
                const uint16_t k = static_cast<uint16_t>((std::min)(left, static_cast<size_t>(128))), op = 0x54;
                const uint8_t hdr[4] = {static_cast<uint8_t>(op), static_cast<uint8_t>(op >> 8), static_cast<uint8_t>(k), static_cast<uint8_t>(k >> 8)};
                out.insert(out.end(), hdr, hdr + 4);
                out.insert(out.end(), in.tris.begin() + at * 30, in.tris.begin() + (at + k) * 30);
                at += k, left -= k;
            }
        }
        out.push_back(0xff), out.push_back(0xff);
        if (c.buf == nullptr || c.size != out.size())
        {
            auto* buf = static_cast<uint8_t*>(::HeapAlloc(::GetProcessHeap(), 0, out.size()));
            if (buf == nullptr)
                return false;
            c.buf = buf, c.size = out.size();
        }
        std::memcpy(c.buf, out.data(), out.size());
        c.key = in.key;
        return true;
    }

    // A copy no live part points at any more (its part was freed or moved on), free to stand in for another stream.
    bool SplitCopyIdle(const SplitCopy& c)
    {
        for (const auto& [part, link] : s_SplitParts)
        {
            if (link.second != c.buf)
                continue;
            uint8_t* now = nullptr;
            if (Readable(reinterpret_cast<const void*>(part + 0x36), 4))
                std::memcpy(&now, reinterpret_cast<const void*>(part + 0x36), 4);
            if (now == c.buf)
                return false;
        }
        return true;
    }

    void SplitPart(const uintptr_t part)
    {
        uint8_t* s = nullptr;
        if (!Readable(reinterpret_cast<const void*>(part + 0x36), 4))
            return;
        std::memcpy(&s, reinterpret_cast<const void*>(part + 0x36), 4);
        if (s == nullptr)
            return;
        // already on a copy of ours: the usual case, every frame
        for (const auto& c : s_SplitCopies)
            if (c.buf == s)
                return;
        // didn't qualify last time, and the part still has the same stream
        const auto seen = s_SplitSeen.find(part);
        if (seen != s_SplitSeen.end() && seen->second == s)
            return;
        SplitInfo in;
        if (!SplitParse(s, in))
        {
            s_SplitSeen[part] = s;
            return;
        }
        s_SplitSeen.erase(part);
        // the copy standing in for this stream; else an idle one built for the same mesh; else a new one
        SplitCopy* c = nullptr;
        for (auto& e : s_SplitCopies)
            if (e.from == s && e.key == in.key)
                c = &e;
        if (c == nullptr)
            for (auto& e : s_SplitCopies)
                if (e.key == in.key && SplitCopyIdle(e))
                    c = &e;
        if (c == nullptr)
        {
            s_SplitCopies.emplace_back();
            c = &s_SplitCopies.back();
        }
        // (re)build from the stream as it is now: the records hold what the game set up for this load
        if (!SplitBuild(in, *c))
            return;
        c->from = s;
        std::memcpy(reinterpret_cast<void*>(part + 0x36), &c->buf, 4);
        s_SplitParts[part] = {s, c->buf};
    }

    // Put the parts back on their own streams (only those still pointing at the copy they were given).
    void SplitUndo(void)
    {
        for (const auto& [part, link] : s_SplitParts)
        {
            uint8_t* now = nullptr;
            if (Readable(reinterpret_cast<const void*>(part + 0x36), 4))
                std::memcpy(&now, reinterpret_cast<const void*>(part + 0x36), 4);
            if (now == link.second)
            {
                const uint8_t* from = link.first;
                std::memcpy(reinterpret_cast<void*>(part + 0x36), &from, 4);
            }
        }
        s_SplitParts.clear();
        s_SplitSeen.clear();
    }
}

const char* Fixes::SetSplit(const bool on)
{
    if (on == m_Split)
        return nullptr;
    if (!on)
    {
        s_SplitOn = false;
        SplitUndo();
        SkinUnuse(2);
        m_Split = false;
        return nullptr;
    }
    const char* why = SkinUse(2);
    s_SplitOn       = why == nullptr;
    m_Split         = s_SplitOn;
    return why;
}

// The game's functions and the three places it is patched: op 0x65's table entry, and skin_part_b's skinning call.
const char* Fixes::DisintInstall(void)
{
    // skin_part_b (sub esp,50h / push ebx,ebp / push esi,edi / mov ebx,ecx / push 0 / mov [esp+20h],ebx / call):
    // it reads the palette with mov eax,[palette] at +20. Its first five bytes may be another plugin's jmp hook,
    // so match from push esi and accept either the original entry or a jmp there.
    const char* why = SkinFind();
    if (why != nullptr)
        return why;
    const uint8_t* fn = s_SkinFn;
    if (fn[20] != 0xA1)
        return "the skin function doesn't read its palette where expected";
    s_Palette = *reinterpret_cast<float** const*>(fn + 21);
    // The per-particle op dispatch: mov eax,[ebp] / and eax,0FFh / dec eax / cmp eax,6Dh / ja rel32 / jmp [eax*4+table]
    static const char dispatch[] = "\x8B\x45\x00\x25\xFF\x00\x00\x00\x48\x83\xF8\x6D\x0F\x87????\xFF\x24\x85";
    const uint8_t* d             = FindBytes(dispatch, sizeof(dispatch) - 1);
    if (d == nullptr)
        return "the particle op dispatch wasn't found";
    auto table = *reinterpret_cast<void** const*>(d + sizeof(dispatch) - 1);
    // A particle's world position: sub esp,0Ch / push esi / mov esi,ecx / lea eax,[esi+54h] / mov ecx,[esi+54h]
    static const char world[] = "\x83\xEC\x0C\x56\x8B\xF1\x8D\x46\x54\x8B\x4E\x54";
    s_WorldPos                = reinterpret_cast<WorldPosFn>(const_cast<uint8_t*>(FindBytes(world, sizeof(world) - 1)));
    if (s_WorldPos == nullptr)
        return "the particle world position function wasn't found";
    // A generator emits on a parent particle: sub esp,4Ch / push ebx,ebp,esi / mov esi,ecx / push edi /
    // mov byte [esp+13h],0 / test byte [esi+0DCh],1 (optional: no smoke without it)
    static const char emit[] = "\x83\xEC\x4C\x53\x55\x56\x8B\xF1\x57\xC6\x44\x24\x13\x00\xF6\x86\xDC\x00\x00\x00\x01";
    s_Emit                   = reinterpret_cast<EmitFn>(const_cast<uint8_t*>(FindBytes(emit, sizeof(emit) - 1)));
    void** slot = table + (0x65 - 1);
    if (!Readable(slot, 4))
        return "op 0x65 isn't the do-nothing case";
    if (*slot != table[0x47 - 1]) // both are the do-nothing case on the PC, unless another plugin took 0x65
        return Readable(*slot, 1) && !InModule(*slot) ? "another plugin already handles op 0x65 (its own disintegration)"
                                                      : "op 0x65 isn't the do-nothing case";
    s_Orig = *slot;
    if (s_Stub == nullptr && (s_Stub = MakeStub(s_Orig)) == nullptr)
        return "couldn't allocate the op stub";
    if ((why = SkinUse(1)) != nullptr)
        return why;
    void* stub = s_Stub;
    if (!Poke(slot, &stub, 4))
    {
        DisintRemove();
        return "couldn't patch the particle op table";
    }
    s_Slot = slot;
    return nullptr;
}

void Fixes::DisintRemove(void)
{
    if (s_Slot != nullptr)
        Poke(s_Slot, &s_Orig, 4);
    s_Slot = nullptr;
    SkinUnuse(1);
    // The stub stays allocated: the interpreter may be inside it on another frame's stack only while it runs, and
    // nothing jumps to it once the table is restored.
    s_Part = nullptr;
    for (void*& vb : s_SkinVb)
        vb = nullptr;
    for (auto& a : s_Actors)
        ReleaseActor(a.second);
    s_Actors.clear();
    for (auto& f : s_Pieces)
        if (f.tex != nullptr)
            f.tex->Release();
    s_Pieces.clear();
    s_Haze.clear();
    if (s_HazeTex != nullptr)
        s_HazeTex->Release();
    s_HazeTex = nullptr;
    s_New.clear();
    s_Spheres.clear();
}

// A part draw of a capturing actor: take the triangles inside this frame's spheres, then draw the rest.
// True: drawn here (or nothing left to draw).
bool Fixes::DisintDraw(const D3DPRIMITIVETYPE t, const UINT minIndex, const UINT numVerts, const UINT startIndex, const UINT count)
{
    if (s_Part == nullptr || (t != D3DPT_TRIANGLELIST && t != D3DPT_TRIANGLESTRIP))
        return false;
    // Only the draws of the part just skinned: its positions are in the buffer it was skinned into.
    IDirect3DVertexBuffer8* pos = nullptr;
    UINT posStride              = 0;
    if (FAILED(m_Device->GetStreamSource(0, &pos, &posStride)) || pos == nullptr)
        return false;
    pos->Release(); // the device keeps it
    const int skinned = posStride != 12 ? -1 : pos == s_SkinVb[0] ? 0 : pos == s_SkinVb[1] ? 1 : -1;
    if (skinned < 0)
        return false;
    IDirect3DIndexBuffer8* ib = nullptr;
    UINT base                 = 0;
    if (FAILED(m_Device->GetIndices(&ib, &base)) || ib == nullptr)
        return false;
    ib->Release(); // the device keeps it
    auto& actor = s_Actors[s_Part];
    // Keyed by the positions too: a mirrored part (the other arm, the other leg) is the same index range drawn
    // again from the skinner's second output, and on the PS2 each side breaks up on its own.
    const uint64_t key = ((static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ib)) << 32) ^ (static_cast<uint64_t>(startIndex) << 12) ^ (static_cast<uint64_t>(count) << 1) ^ (t == D3DPT_TRIANGLESTRIP ? 1 : 0)) ^
                         ((static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pos)) ^ static_cast<uint64_t>(base) << 24) * 0x9E3779B97F4A7C15ull);
    auto& part = actor.parts[key];
    if (part.taken.empty())
    {
        D3DINDEXBUFFER_DESC desc{};
        ib->GetDesc(&desc);
        const UINT n = t == D3DPT_TRIANGLELIST ? count * 3 : count + 2;
        BYTE* p      = nullptr;
        if (desc.Format != D3DFMT_INDEX16 || (startIndex + n) * 2 > desc.Size || FAILED(ib->Lock(startIndex * 2, n * 2, &p, D3DLOCK_READONLY)) || p == nullptr)
        {
            part.taken.assign(1, 1); // marks it read; tri stays empty, so nothing is ever taken
            return false;
        }
        const auto idx = reinterpret_cast<const uint16_t*>(p);
        for (UINT i = 0; i < count; ++i)
        {
            uint16_t a = idx[i * 3], b = idx[i * 3 + 1], c = idx[i * 3 + 2];
            if (t == D3DPT_TRIANGLESTRIP)
            {
                a = idx[i], b = idx[i + 1], c = idx[i + 2];
                if (i & 1)
                    std::swap(a, b);
            }
            if (a == b || b == c || a == c)
                continue;
            part.tri.insert(part.tri.end(), {a, b, c});
        }
        ib->Unlock();
        part.taken.assign((std::max)(static_cast<size_t>(1), part.tri.size() / 3), part.tri.empty() ? 1 : 0);
    }
    const size_t tris = part.tri.size() / 3;

    // Capture: any vertex inside a sphere of this actor. As on the PS2, the sphere grows from the body's centre
    // over the particle's life and takes the body gradually.
    bool any = false;
    for (const auto& sp : s_Spheres)
        any = any || sp.bones == s_Part;
    if (any && part.takenCount < tris)
    {
        IDirect3DVertexBuffer8 *uv = nullptr, *nrm = nullptr;
        UINT uvStride = 0, nrmStride = 0;
        m_Device->GetStreamSource(2, &uv, &uvStride);
        m_Device->GetStreamSource(1, &nrm, &nrmStride);
        D3DVERTEXBUFFER_DESC ud{};
        if (uv != nullptr)
            uv->GetDesc(&ud);
        IDirect3DBaseTexture8* tex = nullptr;
        m_Device->GetTexture(0, &tex);
        LightSet lights;
        DWORD lighting = FALSE;
        m_Device->GetRenderState(D3DRS_LIGHTING, &lighting);
        if (lighting)
        {
            m_Device->GetMaterial(&lights.m);
            DWORD amb = 0;
            m_Device->GetRenderState(D3DRS_AMBIENT, &amb);
            lights.amb[0] = ((amb >> 16) & 0xFF) / 255.0f, lights.amb[1] = ((amb >> 8) & 0xFF) / 255.0f, lights.amb[2] = (amb & 0xFF) / 255.0f;
            for (DWORD li = 0; li < 16 && lights.n < 8; ++li)
            {
                BOOL on = FALSE;
                if (SUCCEEDED(m_Device->GetLightEnable(li, &on)) && on && SUCCEEDED(m_Device->GetLight(li, &lights.l[lights.n])))
                    ++lights.n;
            }
        }
        const UINT verts = static_cast<UINT>(s_SkinPos[skinned].size() / 3);
        const BYTE* pp   = reinterpret_cast<const BYTE*>(s_SkinPos[skinned].data());
        // The normals, skinned with the positions (stream 1); unit length, or it isn't a normal buffer.
        const BYTE* np = nullptr;
        if (lighting && nrm != nullptr && nrmStride == 12 && nrm == s_SkinVb[skinned + 2] && s_SkinPos[skinned + 2].size() == s_SkinPos[skinned].size())
        {
            np              = reinterpret_cast<const BYTE*>(s_SkinPos[skinned + 2].data());
            const float* n0 = reinterpret_cast<const float*>(np);
            const float l2  = n0[0] * n0[0] + n0[1] * n0[1] + n0[2] * n0[2];
            if (l2 < 0.8f || l2 > 1.2f)
                np = nullptr;
        }
        BYTE* up           = nullptr;
        bool uvTried       = false; // locked on the first triangle taken
        const DWORD now    = ::GetTickCount();
        for (size_t i = 0; i < tris; ++i)
        {
            if (part.taken[i])
                continue;
            const float* v[3]{};
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k)
            {
                const UINT vi = base + part.tri[i * 3 + k];
                ok            = vi < verts;
                if (ok)
                    v[k] = reinterpret_cast<const float*>(pp + vi * 12);
            }
            if (!ok)
                continue;
            bool in = false;
            for (const auto& sp : s_Spheres)
                if (sp.bones == s_Part)
                    for (int k = 0; k < 3 && !in; ++k)
                        in = Dist2(v[k], sp.c) < sp.r * sp.r;
            if (!in)
                continue;
            part.taken[i] = 1;
            ++part.takenCount;
            part.dirty = true;
            ++s_Hits;
            // a random sample of four of them, for the smoke and haze
            const uint32_t slot = s_Hits <= 4 ? s_Hits - 1 : static_cast<uint32_t>(Rand() * static_cast<float>(s_Hits));
            if (slot < 4)
                for (int j = 0; j < 3; ++j)
                    s_HitAt[slot][j] = (v[0][j] + v[1][j] + v[2][j]) / 3.0f;
            if (s_Pieces.size() >= 20000)
                continue;
            if (!uvTried && uv != nullptr && uvStride >= 8 && FAILED(uv->Lock(0, 0, &up, D3DLOCK_READONLY)))
                up = nullptr;
            uvTried = true;
            Piece f{};
            for (int k = 0; k < 3; ++k)
            {
                std::memcpy(f.p[k], v[k], 12);
                const UINT vi = base + part.tri[i * 3 + k];
                if (up != nullptr && (vi + 1) * uvStride <= ud.Size)
                    std::memcpy(f.uv[k], up + vi * uvStride, 8);
                if (np != nullptr)
                    f.c[k] = LitColour(lights, v[k], reinterpret_cast<const float*>(np + vi * 12));
            }
            f.lit = np != nullptr;
            // mt00's 'disg' element: a random tumble up to op 0c's rates and a random rise up to op 03's, on top of
            // op 02's, one of each a drawn frame.
            const auto elem = static_cast<uint32_t>(uint64_t{now} * 3 / 100); // 30 Hz frames
            if (elem != s_ElemFrame)
            {
                s_ElemFrame = elem;
                for (int j = 0; j < 3; ++j)
                    s_ElemSpin[j] = PieceSpin[j] * (Rand() * 2.0f - 1.0f);
                s_ElemDrift = PieceDrift * (Rand() * 2.0f - 1.0f);
            }
            std::memcpy(f.spin, s_ElemSpin, sizeof(f.spin));
            f.drift = s_ElemDrift;
            f.tex   = tex;
            f.born  = now;
            if (tex != nullptr)
                tex->AddRef();
            s_Pieces.push_back(f);
        }
        if (up != nullptr)
            uv->Unlock();
        if (tex != nullptr)
            tex->Release();
        if (uv != nullptr)
            uv->Release();
        if (nrm != nullptr)
            nrm->Release();
    }

    // The view this body is drawn with, for the pieces (fixed function).
    {
        void* rt = TargetOf(m_Device);
        size_t i = 0;
        while (i < s_ViewCount && s_Views[i].target != rt)
            ++i;
        if (i == s_ViewCount && i < std::size(s_Views))
        {
            s_Views[i].target = rt;
            m_Device->GetTransform(D3DTS_VIEW, reinterpret_cast<D3DMATRIX*>(s_Views[i].view));
            m_Device->GetTransform(D3DTS_PROJECTION, reinterpret_cast<D3DMATRIX*>(s_Views[i].proj));
            s_ViewCount = i + 1;
        }
    }
    // Form 2 is the same mesh in the same pose, drawn after form 1 (the capturing actor) and opaque: on the PC it
    // wins every depth tie and covers form 1 at once. On the PS2 form 1 stays on top and only its holes show form 2,
    // so a capturing actor's parts are drawn here, a step nearer.
    DWORD bias = 0;
    m_Device->GetRenderState(D3DRS_ZBIAS, &bias);
    m_Device->SetRenderState(D3DRS_ZBIAS, bias + 1);
    if (part.takenCount == 0)
    {
        m_Drawing = true;
        RawDrawIndexedPrimitive(t, minIndex, numVerts, startIndex, count);
        m_Drawing = false;
        m_Device->SetRenderState(D3DRS_ZBIAS, bias);
        return true;
    }
    if (part.dirty)
    {
        part.dirty = false;
        part.kept  = 0;
        if (part.ib == nullptr && FAILED(m_Device->CreateIndexBuffer(static_cast<UINT>(part.tri.size() * 2), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &part.ib)))
            part.ib = nullptr;
        BYTE* p = nullptr;
        if (part.ib != nullptr && SUCCEEDED(part.ib->Lock(0, 0, &p, 0)) && p != nullptr)
        {
            auto out = reinterpret_cast<uint16_t*>(p);
            for (size_t i = 0; i < tris; ++i)
                if (!part.taken[i])
                {
                    std::memcpy(out + part.kept * 3, &part.tri[i * 3], 6);
                    ++part.kept;
                }
            part.ib->Unlock();
        }
    }
    if (part.ib != nullptr && part.kept != 0) // else all gone (or no buffer): skip the draw
    {
        m_Device->SetIndices(part.ib, base);
        m_Drawing = true;
        RawDrawIndexedPrimitive(D3DPT_TRIANGLELIST, minIndex, numVerts, 0, part.kept);
        m_Drawing = false;
        m_Device->SetIndices(ib, base);
    }
    m_Device->SetRenderState(D3DRS_ZBIAS, bias);
    return true;
}

// Every game draw: the pieces go in before the first interface draw (after the 3D world and its effects), as the
// PS2's 'disg' elements, made at the break, draw after the flash and smoke already live.
void Fixes::DisintBeforeDraw(void)
{
    if (s_Drawn || (s_Pieces.empty() && s_Haze.empty()) || s_ViewCount == 0)
        return;
    DWORD vs = 0;
    if (FAILED(m_Device->GetVertexShader(&vs)) || vs != UiFvf)
        return;
    void* rt = TargetOf(m_Device);
    size_t i = 0;
    while (i < s_ViewCount && s_Views[i].target != rt)
        ++i;
    if (i == s_ViewCount) // an offscreen interface-style pass, not the scene
        return;
    std::memcpy(s_View, s_Views[i].view, sizeof(s_View));
    std::memcpy(s_Proj, s_Views[i].proj, sizeof(s_Proj));
    s_Drawn = true;
    DisintDrawPieces();
}

// mt00's 'disg' pieces, solid and textured: each rises 0.012 a tick (op 02, plus up to +-0.003 from op 03), tumbles
// and shrinks away over 40 ticks.
void Fixes::DisintDrawPieces(void)
{
    const DWORD now = ::GetTickCount();
    size_t w        = 0;
    for (size_t i = 0; i < s_Pieces.size(); ++i)
    {
        if ((now - s_Pieces[i].born) * Tick >= PieceLife)
        {
            if (s_Pieces[i].tex != nullptr)
                s_Pieces[i].tex->Release();
            continue;
        }
        s_Pieces[w++] = s_Pieces[i];
    }
    s_Pieces.resize(w);
    w = 0;
    for (const auto& h : s_Haze)
        if ((now - h.born) * Tick < HazeLife)
            s_Haze[w++] = h;
    s_Haze.resize(w);
    if (s_Pieces.empty() && s_Haze.empty())
        return;
    DWORD block = 0;
    if (FAILED(m_Device->CreateStateBlock(D3DSBT_ALL, &block)))
        return;
    const auto rs = [this](D3DRENDERSTATETYPE s, DWORD val) { m_Device->SetRenderState(s, val); };
    // The PS2's piece pass: depth test on, depth write off, no blending.
    rs(D3DRS_ZENABLE, D3DZB_TRUE);
    rs(D3DRS_ZWRITEENABLE, FALSE);
    rs(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    rs(D3DRS_FILLMODE, D3DFILL_SOLID);
    rs(D3DRS_CULLMODE, D3DCULL_NONE);
    rs(D3DRS_LIGHTING, FALSE);
    rs(D3DRS_FOGENABLE, FALSE);
    rs(D3DRS_ALPHABLENDENABLE, FALSE);
    rs(D3DRS_ALPHATESTENABLE, FALSE);
    rs(D3DRS_STENCILENABLE, FALSE);
    rs(D3DRS_COLORWRITEENABLE, 0xF);
    rs(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
    rs(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
    m_Device->SetPixelShader(0);
    for (DWORD st = 1; st < 4; ++st)
        m_Device->SetTexture(st, nullptr);
    const auto ts = [this](D3DTEXTURESTAGESTATETYPE s, DWORD val) { m_Device->SetTextureStageState(0, s, val); };
    ts(D3DTSS_TEXCOORDINDEX, 0);
    ts(D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    ts(D3DTSS_COLOROP, D3DTOP_MODULATE2X);
    ts(D3DTSS_COLORARG1, D3DTA_TEXTURE);
    ts(D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    ts(D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    ts(D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    ts(D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    ts(D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    ts(D3DTSS_MIPFILTER, D3DTEXF_NONE);
    ts(D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    ts(D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    m_Device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    m_Device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    m_Device->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    m_Device->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(id));
    m_Device->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(s_View));
    m_Device->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(s_Proj));

    // One draw per run of the same texture (they arrive part by part).
    std::vector<Vtx> vtx;
    vtx.reserve(3 * 512);
    m_Drawing       = true;
    const auto flush = [&](IDirect3DBaseTexture8* tex) {
        if (vtx.empty())
            return;
        m_Device->SetTexture(0, tex);
        RawDrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(vtx.size() / 3), vtx.data(), sizeof(Vtx));
        vtx.clear();
    };
    IDirect3DBaseTexture8* cur = nullptr;
    for (const auto& f : s_Pieces)
    {
        const float t = (now - f.born) * Tick;
        if (f.tex != cur || vtx.size() >= 3 * 4096)
            flush(cur), cur = f.tex;
        // Height points down, so rising is negative y. The PS2's element matrix scales each piece by op 0f's
        // (0.8, 0.8, 1), shrinking to mtx0's 0.02 over its life, and turns it by its spin (about z, then y, then
        // x). It does so about the piece's corners summed and scaled by 0.333, not quite its centroid.
        const float rise = (PieceRise + f.drift) * t;
        const float k    = 1.0f - (1.0f - PieceEnd) * t / PieceLife;
        const float sc[3]{0.8f * k, 0.8f * k, k};
        float cs[3], sn[3];
        for (int j = 0; j < 3; ++j)
            cs[j] = std::cos(f.spin[j] * t), sn[j] = std::sin(f.spin[j] * t);
        float cen[3];
        for (int j = 0; j < 3; ++j)
            cen[j] = (f.p[0][j] + f.p[1][j] + f.p[2][j]) * 0.333f;
        for (int v = 0; v < 3; ++v)
        {
            float o[3];
            for (int j = 0; j < 3; ++j)
                o[j] = (f.p[v][j] - cen[j]) * sc[j];
            float x = o[0] * cs[2] - o[1] * sn[2], y = o[0] * sn[2] + o[1] * cs[2]; // about z
            o[0] = x, o[1] = y;
            x       = o[0] * cs[1] + o[2] * sn[1];
            float z = o[2] * cs[1] - o[0] * sn[1]; // about y
            o[0] = x, o[2] = z;
            y = o[1] * cs[0] - o[2] * sn[0], z = o[1] * sn[0] + o[2] * cs[0]; // about x
            o[1] = y, o[2] = z;
            vtx.push_back({cen[0] + o[0], cen[1] + o[1] - rise, cen[2] + o[2], f.lit ? f.c[v] : PieceColour, f.uv[v][0], f.uv[v][1]});
        }
    }
    flush(cur);
    if (!s_Haze.empty())
        DisintDrawHaze();
    m_Drawing = false;
    m_Device->ApplyStateBlock(block);
    m_Device->DeleteStateBlock(block);
}

// g000's 'dist' elements, drawn as the PS2 does: a fan of a centre and four corners (a diamond of radius 1, op 0f
// scales it to 0.4) facing the camera, turning 0.0698 radians a tick (op 0c) and rising 0.002 a tick (op 02) for 30
// ticks. It's textured with the frame buffer itself: each vertex reads the picture at its own screen position
// shifted by (0.07, 0.07) in the element's frame, so it shows what's behind it moved a little, the shift turning
// with it. The centre's alpha climbs, holds near 0x78 (of 0x80) and falls; the corners have none. Depth tested, no
// depth writes. Blend (Cs - Cd) As + Cd.
void Fixes::DisintDrawHaze(void)
{
    IDirect3DSurface8* rt = nullptr;
    if (FAILED(m_Device->GetRenderTarget(&rt)) || rt == nullptr)
        return;
    D3DSURFACE_DESC rd{};
    rt->GetDesc(&rd);
    D3DSURFACE_DESC td{};
    if (s_HazeTex != nullptr && (FAILED(s_HazeTex->GetLevelDesc(0, &td)) || td.Width != rd.Width || td.Height != rd.Height || td.Format != rd.Format))
        s_HazeTex->Release(), s_HazeTex = nullptr;
    if (s_HazeTex == nullptr && rd.MultiSampleType == D3DMULTISAMPLE_NONE &&
        FAILED(m_Device->CreateTexture(rd.Width, rd.Height, 1, D3DUSAGE_RENDERTARGET, rd.Format, D3DPOOL_DEFAULT, &s_HazeTex)))
        s_HazeTex = nullptr;
    IDirect3DSurface8* dst = nullptr;
    const bool copied      = s_HazeTex != nullptr && SUCCEEDED(s_HazeTex->GetSurfaceLevel(0, &dst)) && SUCCEEDED(m_Device->CopyRects(rt, nullptr, 0, dst, nullptr));
    if (dst != nullptr)
        dst->Release();
    rt->Release();
    if (!copied)
        return;
    D3DVIEWPORT8 vp{};
    m_Device->GetViewport(&vp);
    const float* V      = s_View;
    const float* P      = s_Proj;
    const auto toView   = [V](const float* p, float* o) {
        for (int j = 0; j < 3; ++j)
            o[j] = p[0] * V[j] + p[1] * V[4 + j] + p[2] * V[8 + j] + V[12 + j];
    };
    // A view-space point's place in the copy.
    const auto uvOf = [&](const float* q, float& u, float& v) {
        const float x  = q[0] * P[0] + q[1] * P[4] + q[2] * P[8] + P[12];
        const float y  = q[0] * P[1] + q[1] * P[5] + q[2] * P[9] + P[13];
        const float w  = q[0] * P[3] + q[1] * P[7] + q[2] * P[11] + P[15];
        const float iw = std::fabs(w) > 1e-6f ? 1.0f / w : 0.0f;
        u              = (vp.X + (x * iw * 0.5f + 0.5f) * vp.Width) / rd.Width;
        v              = (vp.Y + (0.5f - y * iw * 0.5f) * vp.Height) / rd.Height;
    };
    const DWORD now           = ::GetTickCount();
    static const float rim[5][2] = {{0, -1}, {-1, 0}, {0, 1}, {1, 0}, {0, -1}};
    std::vector<Vtx> vtx;
    vtx.reserve(12 * s_Haze.size());
    for (const auto& h : s_Haze)
    {
        const float t     = (now - h.born) * Tick;
        const float u     = t / HazeLife;
        const float a     = std::clamp((std::min)(2.3f * (1.0f - std::fabs(2.0f * u - 1.0f)), 1.0f) * 0.94f, 0.0f, 1.0f);
        const float p[3]  = {h.p[0], h.p[1] - 0.002f * t, h.p[2]}; // height points down
        float c[3];
        toView(p, c);
        const float cs = std::cos(0.0698f * t) * 0.4f, sn = std::sin(0.0698f * t) * 0.4f;
        Vtx fan[6];
        for (int i = 0; i < 6; ++i)
        {
            const float lx = i == 0 ? 0.0f : rim[i - 1][0], ly = i == 0 ? 0.0f : rim[i - 1][1];
            const float sx = lx + HazeShift, sy = ly + HazeShift;
            const float q[3]  = {c[0] + lx * cs - ly * sn, c[1] + lx * sn + ly * cs, c[2]};
            const float qs[3] = {c[0] + sx * cs - sy * sn, c[1] + sx * sn + sy * cs, c[2]};
            fan[i]            = {q[0], q[1], q[2], D3DCOLOR_ARGB(i == 0 ? static_cast<uint32_t>(a * 255.0f) : 0u, 255, 255, 255), 0, 0};
            uvOf(qs, fan[i].u, fan[i].v);
        }
        for (int i = 1; i < 5; ++i)
            vtx.push_back(fan[0]), vtx.push_back(fan[i]), vtx.push_back(fan[i + 1]);
    }
    static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    m_Device->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(id));
    m_Device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    m_Device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_Device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    m_Device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    m_Device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    m_Device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    m_Device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    m_Device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    m_Device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    m_Device->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    m_Device->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    m_Device->SetTexture(0, s_HazeTex);
    RawDrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(vtx.size() / 3), vtx.data(), sizeof(Vtx));
    m_Device->SetTexture(0, nullptr);
}

// Present: this frame's spheres are used by the next frame's draws; an actor with no spheres for a while gets its
// body back.
void Fixes::DisintPresent(void)
{
    s_Drawn     = false;
    s_ViewCount = 0;
    s_Part      = nullptr;
    for (void*& vb : s_SkinVb)
        vb = nullptr;
    if (s_HazeTex != nullptr && m_Device->TestCooperativeLevel() != D3D_OK)
        s_HazeTex->Release(), s_HazeTex = nullptr;
    s_LastHits = s_Hits;
    std::memcpy(s_LastHitAt, s_HitAt, sizeof(s_HitAt));
    s_Hits = 0;
    s_Spheres.swap(s_New);
    s_New.clear();
    const DWORD now = ::GetTickCount();
    for (const auto& sp : s_Spheres)
        s_Actors[sp.bones].lastSphere = now;
    for (auto it = s_Actors.begin(); it != s_Actors.end();)
        if (now - it->second.lastSphere > HoldMs)
        {
            ReleaseActor(it->second);
            it = s_Actors.erase(it);
        }
        else
            ++it;
}
