// Mouse offset in a window. In windowed modes the game's mouse code scales the cursor by the window rect over the rect
// less the theme's frame and caption (GetSystemMetrics SM_CXFRAME, SM_CYFRAME, SM_CYCAPTION): y * height / (height -
// 2 * frame - caption). When the window's real frame isn't that size (none at all under Wine), the cursor reads too
// far down and right, more so further from the origin, and menu hover zones sit above the buttons. The fix answers
// those three with the window's real frame, window rect less client rect, which makes the game's sum exact.
#include "fixes.h"
#include "debuglog.h"

#include <cstring>

namespace
{
    using MetricsFn = int(WINAPI*)(int);

    MetricsFn s_Metrics = nullptr; // the game's GetSystemMetrics, while patched
    void** s_Slot       = nullptr; // its slot in FFXiMain's import table
    HWND s_Window       = nullptr;

    int WINAPI MetricsHook(const int index)
    {
        RECT window{}, client{};
        if ((index != SM_CXFRAME && index != SM_CYFRAME && index != SM_CYCAPTION) || !::GetWindowRect(s_Window, &window) || !::GetClientRect(s_Window, &client))
            return s_Metrics(index);
        const int wide = (window.right - window.left) - client.right;
        const int high = (window.bottom - window.top) - client.bottom;
        DebugLog::Once("mouse", "mouse: real frame %d x %d, the system says frame %d x %d, caption %d", wide, high, s_Metrics(SM_CXFRAME), s_Metrics(SM_CYFRAME), s_Metrics(SM_CYCAPTION));
        if (index == SM_CXFRAME)
            return wide / 2;
        return index == SM_CYFRAME ? high / 2 : high % 2; // the game takes 2 * frame + caption from the height
    }

    void PokeSlot(void** slot, void* value)
    {
        DWORD old = 0;
        if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
        {
            *slot = value;
            ::VirtualProtect(slot, sizeof(void*), old, &old);
        }
    }

    // GetSystemMetrics' slot in FFXiMain's import table.
    void** FindSlot(void)
    {
        const auto base = reinterpret_cast<const BYTE*>(::GetModuleHandleA("FFXiMain.dll"));
        if (base == nullptr)
            return nullptr;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress == 0)
            return nullptr;
        for (auto imp = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name != 0; ++imp)
        {
            if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), "user32.dll") != 0 || imp->OriginalFirstThunk == 0)
                continue;
            auto name = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
            auto slot = reinterpret_cast<IMAGE_THUNK_DATA*>(const_cast<BYTE*>(base) + imp->FirstThunk);
            for (; name->u1.AddressOfData != 0; ++name, ++slot)
                if (!IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal) &&
                    std::strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name->u1.AddressOfData)->Name, "GetSystemMetrics") == 0)
                    return reinterpret_cast<void**>(&slot->u1.Function);
        }
        return nullptr;
    }
}

const char* Fixes::SetMouse(const bool on)
{
    if (on == m_Mouse)
        return nullptr;
    if (!on)
    {
        if (s_Slot != nullptr && *s_Slot == reinterpret_cast<void*>(&MetricsHook))
            PokeSlot(s_Slot, reinterpret_cast<void*>(s_Metrics));
        s_Slot  = nullptr;
        m_Mouse = false;
        return nullptr;
    }
    if (m_Device == nullptr)
        return "no device yet";
    D3DDEVICE_CREATION_PARAMETERS params{};
    if (FAILED(m_Device->GetCreationParameters(&params)) || params.hFocusWindow == nullptr)
        return "the game's window wasn't found";
    void** slot = FindSlot();
    if (slot == nullptr)
        return "the game's GetSystemMetrics import wasn't found";
    s_Window  = params.hFocusWindow;
    s_Metrics = reinterpret_cast<MetricsFn>(*slot);
    s_Slot    = slot;
    PokeSlot(slot, reinterpret_cast<void*>(&MetricsHook));
    m_Mouse = true;
    PF_DEBUG("mouse: on, window style %08lx", static_cast<unsigned long>(::GetWindowLongA(s_Window, GWL_STYLE)));
    return nullptr;
}
