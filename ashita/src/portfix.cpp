// portfix for Ashita v4: forwards the Direct3D draw events to the fixes in core/ and handles /portfix.
#define _CRT_SECURE_NO_WARNINGS
#include "Ashita.h"
#include "fixes.h"

#include <string>
#include <vector>

class PortFix final : public IPlugin, public Fixes
{
    IAshitaCore* m_AshitaCore = nullptr;

    // The device calls come back through Ashita's hooks; Fixes ignores them while it draws.
    void RawDrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count) override { m_Device->DrawPrimitive(t, start, count); }
    void RawDrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, const void* data, UINT stride) override { m_Device->DrawPrimitiveUP(t, count, data, stride); }
    void RawDrawIndexedPrimitive(D3DPRIMITIVETYPE t, UINT minIndex, UINT numVerts, UINT startIndex, UINT count) override
    {
        m_Device->DrawIndexedPrimitive(t, minIndex, numVerts, startIndex, count);
    }
    void Print(const std::string& msg) const
    {
        m_AshitaCore->GetChatManager()->Write(1, false, (Ashita::Chat::Header("portfix") + Ashita::Chat::Message(msg)).c_str());
    }

public:
    const char* GetName(void) const override { return "portfix"; }
    const char* GetAuthor(void) const override { return "voetre"; }
    const char* GetDescription(void) const override { return "Fixes for graphics the PC port draws wrong."; }
    const char* GetLink(void) const override { return "https://github.com/voetre/portfix"; }
    double GetVersion(void) const override { return 1.1; }
    // Early, so the fixes see the game's draws before other plugins act on them.
    int32_t GetPriority(void) const override { return -10; }
    uint32_t GetFlags(void) const override { return static_cast<uint32_t>(Ashita::PluginFlags::UseCommands | Ashita::PluginFlags::UseDirect3D); }

    bool Initialize(IAshitaCore* core, ILogManager* logger, const uint32_t id) override
    {
        m_AshitaCore = core;
        return true;
    }
    void Release(void) override { Fixes::Release(); }
    bool Direct3DInitialize(IDirect3DDevice8* device) override
    {
        m_Device = device;
        if (const char* error = SetDisint(true))
            Print(std::string("disint - off: ") + error);
        return true;
    }
    void Direct3DPresent(const RECT*, const RECT*, HWND, const RGNDATA*) override { OnPresent(); }
    bool Direct3DDrawPrimitive(D3DPRIMITIVETYPE t, UINT start, UINT count) override { return OnDrawPrimitive(t, start, count); }
    bool Direct3DDrawIndexedPrimitive(D3DPRIMITIVETYPE t, UINT minIndex, UINT numVerts, UINT startIndex, UINT count) override
    {
        return OnDrawIndexedPrimitive(t, minIndex, numVerts, startIndex, count);
    }
    bool Direct3DDrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT count, CONST void* data, UINT stride) override { return OnDrawPrimitiveUP(t, count, data, stride); }

    // /portfix [help] | /portfix <water|halo|shine|disint> [on|off]
    bool HandleCommand(int32_t mode, const char* command, bool injected) override
    {
        std::vector<std::string> args;
        if (Ashita::Commands::GetCommandArgs(command, &args) == 0 || !Ashita::Commands::CommandCheck(args[0], {"/portfix"}))
            return false;
        const std::string sub = args.size() > 1 ? args[1] : "";
        const std::string arg = args.size() > 2 ? args[2] : "";
        if (sub == "water" || sub == "halo" || sub == "shine")
        {
            bool& on = sub == "water" ? m_Water : sub == "halo" ? m_Halo : m_Shine;
            on       = arg == "on" ? true : arg == "off" ? false : !on;
            if (sub == "water")
                SetWater(on);
            Print(sub + " - " + (on ? "on" : "off"));
        }
        else if (sub == "disint")
        {
            const bool on = arg == "on" ? true : arg == "off" ? false : !m_Disint;
            if (const char* error = SetDisint(on))
                Print(std::string("disint - off: ") + error);
            else
                Print(std::string("disint - ") + (m_Disint ? "on" : "off"));
        }
        else if (sub.empty())
        {
            Print(std::string("water - ") + (m_Water ? "on" : "off"));
            Print(std::string("halo - ") + (m_Halo ? "on" : "off"));
            Print(std::string("shine - ") + (m_Shine ? "on" : "off"));
            Print(std::string("disint - ") + (m_Disint ? "on" : "off"));
        }
        else
        {
            Print("/portfix - list fixes and their state");
            Print("/portfix water [on|off] - black water fix");
            Print("/portfix halo [on|off] - summon halo fix");
            Print("/portfix shine [on|off] - sphere-map shine fix");
            Print("/portfix disint [on|off] - disintegrate effect fix");
            Print("/portfix help - this list");
        }
        return true;
    }
};

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
