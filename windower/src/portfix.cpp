// portfix for Windower 4: a Lua C module loaded by addons/portfix/portfix.lua. Windower wraps the Direct3D 8 device,
// so the draw calls are taken from the vtable of the real device under the wrapper and forwarded to the fixes in core/.
#include "fixes.h"

#include <atomic>

namespace
{
    // IDirect3DDevice8 vtable slots.
    enum Slot
    {
        SlotPresent              = 15,
        SlotDrawPrimitive        = 70,
        SlotDrawIndexedPrimitive = 71,
        SlotDrawPrimitiveUP      = 72,
    };

    using PresentFn              = HRESULT(WINAPI*)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using DrawPrimitiveFn        = HRESULT(WINAPI*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
    using DrawIndexedPrimitiveFn = HRESULT(WINAPI*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
    using DrawPrimitiveUPFn      = HRESULT(WINAPI*)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, const void*, UINT);

    PresentFn              g_Present;
    DrawPrimitiveFn        g_DrawPrimitive;
    DrawIndexedPrimitiveFn g_DrawIndexedPrimitive;
    DrawPrimitiveUPFn      g_DrawPrimitiveUP;

    // Set from the Lua thread, applied on the render thread at Present.
    std::atomic<bool> g_WantWater{true};
    std::atomic<bool> g_WantHalo{true};

    class WindowerFixes final : public Fixes
    {
        void RawDrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count) override { g_DrawPrimitive(m_Device, t, start, count); }
        void RawDrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride) override { g_DrawPrimitiveUP(m_Device, t, count, data, stride); }
    };
    WindowerFixes g_Fixes;

    // Other objects of the same class pass through.
    bool GameDevice(IDirect3DDevice8* d)
    {
        return d == g_Fixes.m_Device;
    }

    HRESULT WINAPI HookPresent(IDirect3DDevice8* d, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
    {
        if (GameDevice(d))
        {
            g_Fixes.OnPresent();
            if (const bool w = g_WantWater; w != g_Fixes.m_Water)
                g_Fixes.SetWater(w);
            g_Fixes.m_Halo = g_WantHalo;
        }
        return g_Present(d, src, dst, wnd, dirty);
    }

    HRESULT WINAPI HookDrawPrimitive(IDirect3DDevice8* d, D3DPRIMITIVETYPE t, UINT start, UINT count)
    {
        if (GameDevice(d) && g_Fixes.OnDrawPrimitive(t, start, count))
            return D3D_OK;
        return g_DrawPrimitive(d, t, start, count);
    }

    HRESULT WINAPI HookDrawIndexedPrimitive(IDirect3DDevice8* d, D3DPRIMITIVETYPE t, UINT min, UINT num, UINT start, UINT count)
    {
        if (GameDevice(d))
            g_Fixes.OnDrawIndexedPrimitive();
        return g_DrawIndexedPrimitive(d, t, min, num, start, count);
    }

    HRESULT WINAPI HookDrawPrimitiveUP(IDirect3DDevice8* d, D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride)
    {
        if (GameDevice(d) && g_Fixes.OnDrawPrimitiveUP(t, count, data, stride))
            return D3D_OK;
        return g_DrawPrimitiveUP(d, t, count, data, stride);
    }

    template <class T>
    void Patch(void** vt, const Slot slot, const T hook, T& orig)
    {
        DWORD old = 0;
        VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old);
        orig     = reinterpret_cast<T>(vt[slot]); // set before the slot, the render thread may call the hook at once
        vt[slot] = reinterpret_cast<void*>(hook);
        VirtualProtect(&vt[slot], sizeof(void*), old, &old);
    }

    // FFXiMain.dll keeps its renderer in a global and the device at renderer + 8. Under a launcher this is the
    // launcher's wrapper, the object the game makes its calls on. The global is found from the code that sets it:
    //   push 478h / mov byte [esp+0Bh], 0 / call new / add esp, 4 / test eax, eax / jz ... / mov ecx, eax /
    //   call ctor / test eax, eax / mov [global], eax
    IDirect3DDevice8* FindDevice(void)
    {
        static const char pattern[] = "\x68\x78\x04\x00\x00\xC6\x44\x24\x0B\x00\xE8????\x83\xC4\x04\x85\xC0\x0F\x84????\x8B\xC8\xE8????\x85\xC0\xA3";
        const size_t length = sizeof(pattern) - 1;
        const auto base     = reinterpret_cast<const BYTE*>(GetModuleHandleW(L"FFXiMain.dll"));
        if (base == nullptr)
            return nullptr;
        const auto nt      = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        {
            if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                continue;
            const BYTE* p   = base + section[i].VirtualAddress;
            const BYTE* end = p + section[i].Misc.VirtualSize - length - 4;
            for (; p < end; ++p)
            {
                size_t n = 0;
                while (n < length && (pattern[n] == '?' || p[n] == static_cast<BYTE>(pattern[n])))
                    ++n;
                if (n < length)
                    continue;
                const auto renderer = **reinterpret_cast<BYTE** const*>(p + length);
                return renderer ? *reinterpret_cast<IDirect3DDevice8**>(renderer + 8) : nullptr;
            }
        }
        return nullptr;
    }

    // The vtable is patched in place: Present and the draw calls of the game's device class. The patch is never
    // removed, so the dll is pinned. Windower frees it when the addon unloads.
    // Returns nullptr when the hooks are in place, or the reason they are not.
    const char* Install(void)
    {
        static const char* result = "not tried";
        static bool tried         = false;
        if (tried)
            return result;
        tried = true;

        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Install), &self))
            return result = "pin failed";
        IDirect3DDevice8* device = FindDevice();
        if (device == nullptr)
            return result = "device not found";
        g_Fixes.m_Device = device;
        void** vt        = *reinterpret_cast<void***>(device);
        Patch(vt, SlotPresent, &HookPresent, g_Present);
        Patch(vt, SlotDrawPrimitive, &HookDrawPrimitive, g_DrawPrimitive);
        Patch(vt, SlotDrawIndexedPrimitive, &HookDrawIndexedPrimitive, g_DrawIndexedPrimitive);
        Patch(vt, SlotDrawPrimitiveUP, &HookDrawPrimitiveUP, g_DrawPrimitiveUP);
        return result = nullptr;
    }

    // Lua 5.1 API, taken from Windower's LuaCore.dll at run time so the dll has no link-time Lua dependency.
    struct lua_State;
    using lua_CFunction = int (*)(lua_State*);
    struct luaL_Reg
    {
        const char*   name;
        lua_CFunction func;
    };
    void (*lua_createtable)(lua_State*, int, int);
    void (*luaL_register)(lua_State*, const char*, const luaL_Reg*);
    int (*lua_toboolean)(lua_State*, int);
    void (*lua_pushboolean)(lua_State*, int);
    void (*lua_pushstring)(lua_State*, const char*);

    template <class T>
    bool Import(const HMODULE m, const char* name, T& fn)
    {
        fn = reinterpret_cast<T>(GetProcAddress(m, name));
        return fn != nullptr;
    }

    // start() -> true, or false and the reason
    int LuaStart(lua_State* L)
    {
        const char* error = Install();
        lua_pushboolean(L, error == nullptr);
        if (error == nullptr)
            return 1;
        lua_pushstring(L, error);
        return 2;
    }

    // set(water, halo)
    int LuaSet(lua_State* L)
    {
        g_WantWater = lua_toboolean(L, 1) != 0;
        g_WantHalo  = lua_toboolean(L, 2) != 0;
        return 0;
    }
}

// Plain C entry points, for loading the dll without Lua (test harnesses).
extern "C" __declspec(dllexport) int portfix_start(void)
{
    return Install() == nullptr ? 1 : 0;
}

extern "C" __declspec(dllexport) void portfix_set(const int water, const int halo)
{
    g_WantWater = water != 0;
    g_WantHalo  = halo != 0;
}

// package.loadlib(path, "luaopen_portfix")() -> { start = fn, set = fn }
extern "C" __declspec(dllexport) int luaopen_portfix(lua_State* L)
{
    const HMODULE lua = GetModuleHandleW(L"LuaCore.dll");
    if (lua == nullptr || !Import(lua, "lua_createtable", lua_createtable) || !Import(lua, "luaL_register", luaL_register) ||
        !Import(lua, "lua_toboolean", lua_toboolean) || !Import(lua, "lua_pushboolean", lua_pushboolean) || !Import(lua, "lua_pushstring", lua_pushstring))
        return 0;
    static const luaL_Reg funcs[] = {{"start", LuaStart}, {"set", LuaSet}, {nullptr, nullptr}};
    lua_createtable(L, 0, 2);
    luaL_register(L, nullptr, funcs);
    return 1;
}
