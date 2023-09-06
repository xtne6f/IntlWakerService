#define NOMINMAX
#include <windows.h>
#include <winsvc.h>
#include <wchar.h>
#include <algorithm>
#include <vector>

#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL 0xC0000023
#endif

namespace
{
typedef NTSTATUS (WINAPI PowerInformationWithPrivileges)(
    POWER_INFORMATION_LEVEL InformationLevel,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength);

enum REQUESTER_TYPE {
    KernelRequester,
    UserProcessRequester,
    UserSharedServiceRequester
};

struct DIAGNOSTIC_BUFFER {
    ULONG Size;
    ULONG Padding_;
    ULONG CallerType;
    ULONG Padding2_;
    ULONG ProcessImageNameOffset;
    ULONG Padding3_;
    ULONG ProcessId;
    ULONG ServiceTag;
    ULONG ReasonOffset;
    ULONG Padding4_;
};

struct WAKE_TIMER_INFO {
    ULONG OffsetToNext;
    ULARGE_INTEGER DueTime;
    ULONG Period;
    ULONG Padding_;
    DIAGNOSTIC_BUFFER ReasonContext;
};

struct WAKE_TIMER_LIST_INFO {
    ULONGLONG dueTime;
    ULONG processId;
};

bool ParseWakeTimerList(std::vector<WAKE_TIMER_LIST_INFO> &list, std::vector<BYTE> &buf)
{
    list.clear();

    for (size_t offset = 0;;) {
        if (offset + sizeof(WAKE_TIMER_INFO) > buf.size()) {
            return false;
        }
        WAKE_TIMER_INFO &info = *reinterpret_cast<WAKE_TIMER_INFO*>(buf.data() + offset);
        WAKE_TIMER_LIST_INFO listInfo;
        listInfo.dueTime = info.DueTime.QuadPart;
        listInfo.processId = 0;

        DIAGNOSTIC_BUFFER &context = info.ReasonContext;
        size_t contextOffset = reinterpret_cast<BYTE*>(&context) - buf.data();
        if (context.Size >= sizeof(DIAGNOSTIC_BUFFER) &&
            context.Size <= buf.size() &&
            contextOffset + context.Size <= buf.size())
        {
            if (context.CallerType == UserProcessRequester ||
                context.CallerType == UserSharedServiceRequester)
            {
                listInfo.processId = context.ProcessId;
            }
        }
        list.push_back(listInfo);
        if (!info.OffsetToNext) {
            break;
        }
        if (info.OffsetToNext > buf.size()) {
            return false;
        }
        offset += info.OffsetToNext;
    }

    std::sort(list.begin(), list.end(), [](const WAKE_TIMER_LIST_INFO &a, const WAKE_TIMER_LIST_INFO &b) { return a.dueTime < b.dueTime; });
    return true;
}

HANDLE g_hStopEvent;
SERVICE_STATUS_HANDLE g_hStatusHandle;
LONG g_processing;
LONG g_pauseCountBy2Seconds;

DWORD WINAPI ServiceCtrl(DWORD dwControl, DWORD dwEventType, LPVOID, LPVOID)
{
    if (dwControl == SERVICE_CONTROL_STOP) {
        SERVICE_STATUS ss = {};
        ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        ss.dwCurrentState = SERVICE_STOP_PENDING;
        ss.dwWaitHint = 10000;
        SetServiceStatus(g_hStatusHandle, &ss);
        SetEvent(g_hStopEvent);
        return NO_ERROR;
    }
    else if (dwControl == SERVICE_CONTROL_POWEREVENT) {
        // In some environments, handling a wake timer during resuming
        // causes Kernel-Power 41 crash, so pause the process.
        if (dwEventType == PBT_APMSUSPEND) {
            // Suspending, pause for 60 seconds.
            InterlockedExchange(&g_pauseCountBy2Seconds, 30);
            while (InterlockedExchangeAdd(&g_processing, 0)) {
                Sleep(1);
            }
        }
        else if (dwEventType == PBT_APMRESUMEAUTOMATIC) {
            // Resuming, pause for 10 seconds.
            InterlockedExchange(&g_pauseCountBy2Seconds, 5);
        }
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI ServiceMain(DWORD, LPWSTR *)
{
    g_hStatusHandle = RegisterServiceCtrlHandlerExW(L"IntlWakerService", ServiceCtrl, nullptr);
    if (!g_hStatusHandle) {
        return;
    }
    SERVICE_STATUS ss = {};
    ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ss.dwCurrentState = SERVICE_START_PENDING;
    ss.dwWaitHint = 10000;
    SetServiceStatus(g_hStatusHandle, &ss);

    WCHAR iniPath[MAX_PATH + 3];
    DWORD iniPathLen = GetModuleFileNameW(nullptr, iniPath, MAX_PATH);
    if (iniPathLen == 0 || iniPathLen >= MAX_PATH) {
        ss.dwCurrentState = SERVICE_STOPPED;
        ss.dwWaitHint = 0;
        SetServiceStatus(g_hStatusHandle, &ss);
        return;
    }
    while (--iniPathLen > 0 &&
           iniPath[iniPathLen] != L'/' &&
           iniPath[iniPathLen] != L'\\' &&
           iniPath[iniPathLen] != L'.')
    {
        iniPath[iniPathLen] = 0;
    }
    if (iniPath[iniPathLen] != L'.') {
        ss.dwCurrentState = SERVICE_STOPPED;
        ss.dwWaitHint = 0;
        SetServiceStatus(g_hStatusHandle, &ss);
        return;
    }
    wcscat_s(iniPath, L"ini");

    ULONG minimumTimeSpanHours = GetPrivateProfileIntW(L"Settings", L"MinimumTimeSpanHours", 3, iniPath);
    minimumTimeSpanHours = std::max(minimumTimeSpanHours, 1UL);
    ULONG subtractMsecPerHour = GetPrivateProfileIntW(L"Settings", L"SubtractMsecPerHour", 25000, iniPath);

    HMODULE hModule = LoadLibraryW(L"powrprof.dll");
    if (!hModule) {
        ss.dwCurrentState = SERVICE_STOPPED;
        ss.dwWaitHint = 0;
        SetServiceStatus(g_hStatusHandle, &ss);
        return;
    }

    PowerInformationWithPrivileges *pPowerInformationWithPrivileges =
        reinterpret_cast<PowerInformationWithPrivileges*>(GetProcAddress(hModule, "PowerInformationWithPrivileges"));
    if (pPowerInformationWithPrivileges) {
        ss.dwCurrentState = SERVICE_RUNNING;
        ss.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_POWEREVENT;
        ss.dwWaitHint = 0;
        SetServiceStatus(g_hStatusHandle, &ss);

        std::vector<WAKE_TIMER_LIST_INFO> list;
        std::vector<BYTE> buf;
        HANDLE hResumeTimer = nullptr;
        LONGLONG resumeTime = 0;
        while (WaitForSingleObject(g_hStopEvent, 2000) == WAIT_TIMEOUT) {
            if (InterlockedDecrement(&g_pauseCountBy2Seconds) >= 0) {
                continue;
            }
            InterlockedIncrement(&g_pauseCountBy2Seconds);

            InterlockedExchange(&g_processing, TRUE);
            buf.assign(8192, 0);
            NTSTATUS ret;
            for (;;) {
                ret = pPowerInformationWithPrivileges(WakeTimerList, nullptr, 0, buf.data(), static_cast<ULONG>(buf.size()));
                if (ret != STATUS_BUFFER_TOO_SMALL || buf.size() >= 1024 * 1024) {
                    break;
                }
                buf.assign(buf.size() * 2, 0);
            }

            bool setResume = false;
            if (ret == 0 && ParseWakeTimerList(list, buf)) {
                ULONGLONG lastDueTime = 0;
                for (size_t i = 0; i < list.size(); ++i) {
                    if (list[i].processId == GetCurrentProcessId()) {
                        continue;
                    }
                    ULONGLONG span = list[i].dueTime - lastDueTime;
                    if (span / 10000000 / 3600 < minimumTimeSpanHours) {
                        lastDueTime = list[i].dueTime;
                        continue;
                    }
                    FILETIME ftNow;
                    GetSystemTimeAsFileTime(&ftNow);
                    LARGE_INTEGER liTime;
                    liTime.LowPart = ftNow.dwLowDateTime;
                    liTime.HighPart = ftNow.dwHighDateTime;
                    liTime.QuadPart += list[i].dueTime - span / 3600000 * subtractMsecPerHour;
                    if (!hResumeTimer ||
                        liTime.QuadPart - resumeTime > 20000000 ||
                        resumeTime - liTime.QuadPart > 20000000)
                    {
                        if (!hResumeTimer) {
                            hResumeTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
                        }
                        if (hResumeTimer && SetWaitableTimer(hResumeTimer, &liTime, 0, nullptr, nullptr, TRUE)) {
                            resumeTime = liTime.QuadPart;
                        }
                    }
                    setResume = true;
                    break;
                }
            }
            if (hResumeTimer && !setResume) {
                CloseHandle(hResumeTimer);
                hResumeTimer = nullptr;
            }
            InterlockedExchange(&g_processing, FALSE);
        }
        if (hResumeTimer) {
            CloseHandle(hResumeTimer);
        }
    }
    FreeLibrary(hModule);

    ss.dwCurrentState = SERVICE_STOPPED;
    ss.dwControlsAccepted = 0;
    ss.dwWaitHint = 0;
    SetServiceStatus(g_hStatusHandle, &ss);
}
}

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    SetDllDirectoryW(L"");

    g_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_hStopEvent) {
        WCHAR serviceName[] = L"IntlWakerService";
        SERVICE_TABLE_ENTRYW dispatchTable[] = {
            { serviceName, ServiceMain },
            { nullptr, nullptr }
        };
        StartServiceCtrlDispatcherW(dispatchTable);
        CloseHandle(g_hStopEvent);
    }
    return 0;
}
