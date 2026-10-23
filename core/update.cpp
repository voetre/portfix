#include "update.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdlib>
#include <string>

namespace
{
    constexpr wchar_t Host[] = L"api.github.com";
    constexpr wchar_t Path[] = L"/repos/voetre/portfix/releases/latest";
    constexpr char Page[]    = "https://github.com/voetre/portfix/releases/latest";

    std::atomic<bool> s_Running{false};
    std::atomic<int> s_State{2}; // 0: running, 1: a message waits, 2: over
    std::string s_Message;       // written before s_State becomes 1

    // The body of a GET, or empty on any failure.
    std::string Fetch(void)
    {
        std::string body;
        HINTERNET session = ::WinHttpOpen(L"portfix/" PORTFIX_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET connect = session != nullptr ? ::WinHttpConnect(session, Host, INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;
        HINTERNET request = connect != nullptr ? ::WinHttpOpenRequest(connect, L"GET", Path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
        if (request != nullptr)
        {
            ::WinHttpSetTimeouts(request, 5000, 5000, 5000, 5000);
            DWORD status = 0, size = sizeof(status);
            if (::WinHttpSendRequest(request, L"Accept: application/vnd.github+json", static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                ::WinHttpReceiveResponse(request, nullptr) &&
                ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 200)
            {
                char buf[4096];
                DWORD got = 0;
                while (body.size() < 1 << 20 && ::WinHttpReadData(request, buf, sizeof(buf), &got) && got != 0)
                    body.append(buf, got);
            }
        }
        for (HINTERNET h : {request, connect, session})
            if (h != nullptr)
                ::WinHttpCloseHandle(h);
        return body;
    }

    // "1.10" > "1.9": compared number by number, a missing number counting as 0.
    bool Newer(const char* a, const char* b)
    {
        for (int n = 0; n < 8; ++n)
        {
            char* ea      = nullptr;
            char* eb      = nullptr;
            const long na = std::strtol(a, &ea, 10);
            const long nb = std::strtol(b, &eb, 10);
            if (na != nb)
                return na > nb;
            if (*ea != '.' && *eb != '.')
                return false;
            a = *ea == '.' ? ea + 1 : ea;
            b = *eb == '.' ? eb + 1 : eb;
        }
        return false;
    }

    void Check(void)
    {
        const std::string body = Fetch();
        // "tag_name": "v1.2"
        const size_t key = body.find("\"tag_name\"");
        const size_t open  = key != std::string::npos ? body.find('"', body.find(':', key) + 1) : std::string::npos;
        const size_t close = open != std::string::npos ? body.find('"', open + 1) : std::string::npos;
        if (close != std::string::npos)
        {
            std::string tag = body.substr(open + 1, close - open - 1);
            if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
                tag.erase(0, 1);
            if (!tag.empty() && tag.size() < 32 && Newer(tag.c_str(), PORTFIX_VERSION))
            {
                s_Message = "Version " + tag + " is out (you have " PORTFIX_VERSION "): " + Page;
                s_State   = 1;
                s_Running = false;
                return;
            }
        }
        s_State   = 2;
        s_Running = false;
    }

    // The thread holds a reference on the dll, so an unload while it runs doesn't pull its code away.
    DWORD WINAPI Run(void* self)
    {
        Check();
        ::FreeLibraryAndExitThread(static_cast<HMODULE>(self), 0);
        return 0;
    }
}

void Update::Start(void)
{
    // One request per load. The Windower dll stays loaded across addon reloads, so a finished check doesn't stop the
    // next load's; one still running does.
    if (s_Running.exchange(true))
        return;
    s_State      = 0;
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Run), &self))
    {
        s_State   = 2;
        s_Running = false;
        return;
    }
    HANDLE t = ::CreateThread(nullptr, 0, Run, self, 0, nullptr);
    if (t == nullptr)
    {
        ::FreeLibrary(self);
        s_State   = 2;
        s_Running = false;
        return;
    }
    ::CloseHandle(t);
}

const char* Update::Take(void)
{
    int one = 1;
    return s_State.compare_exchange_strong(one, 2) ? s_Message.c_str() : nullptr;
}

bool Update::Done(void)
{
    return s_State != 0;
}
