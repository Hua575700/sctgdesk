// CustomAction.cpp : Defines the entry point for the custom action.
#include "pch.h"
#include <stdlib.h>
#include <strutil.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <netfw.h>
#include <shlwapi.h>

#include "./Common.h"

#pragma comment(lib, "Shlwapi.lib")

UINT __stdcall CustomActionHello(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    hr = WcaInitialize(hInstall, "CustomActionHello");
    ExitOnFailure(hr, "Failed to initialize");

    WcaLog(LOGMSG_STANDARD, "Initialized.");

    // TODO: Add your custom action code here.
    WcaLog(LOGMSG_STANDARD, "================= Example CustomAction Hello");

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall RemoveInstallFolder(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    int nResult = 0;
    LPWSTR installFolder = NULL;
    LPWSTR pwz = NULL;
    LPWSTR pwzData = NULL;

    hr = WcaInitialize(hInstall, "RemoveInstallFolder");
    ExitOnFailure(hr, "Failed to initialize");

    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");

    pwz = pwzData;
    hr = WcaReadStringFromCaData(&pwz, &installFolder);
    ExitOnFailure(hr, "failed to read database key from custom action data: %ls", pwz);

    SHFILEOPSTRUCTW fileOp;
    ZeroMemory(&fileOp, sizeof(SHFILEOPSTRUCT));

    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = installFolder;
    fileOp.fFlags = FOF_NOCONFIRMATION | FOF_SILENT;

    nResult = SHFileOperationW(&fileOp);
    if (nResult == 0)
    {
        WcaLog(LOGMSG_STANDARD, "The directory \"%ls\" has been deleted.", installFolder);
    }
    else
    {
        WcaLog(LOGMSG_STANDARD, "The directory \"%ls\" has not been deleted, error code: 0X%02X. Please refer to https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shfileoperationa for the error codes.", installFolder, nResult);
    }

LExit:
    ReleaseStr(installFolder);

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

// https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntqueryinformationprocess
// **NtQueryInformationProcess** may be altered or unavailable in future versions of Windows.
// Applications should use the alternate functions listed in this topic.
// But I do not find the alternate functions.
// https://github.com/heim-rs/heim/issues/105#issuecomment-683647573
typedef NTSTATUS(NTAPI *pfnNtQueryInformationProcess)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
bool TerminateProcessIfNotContainsParam(pfnNtQueryInformationProcess NtQueryInformationProcess, HANDLE process, LPCWSTR excludeParam)
{
    bool processClosed = false;
    PROCESS_BASIC_INFORMATION processInfo;
    NTSTATUS status = NtQueryInformationProcess(process, ProcessBasicInformation, &processInfo, sizeof(processInfo), NULL);
    if (status == 0 && processInfo.PebBaseAddress != NULL)
    {
        PEB peb;
        SIZE_T dwBytesRead;
        if (ReadProcessMemory(process, processInfo.PebBaseAddress, &peb, sizeof(peb), &dwBytesRead))
        {
            RTL_USER_PROCESS_PARAMETERS pebUpp;
            if (ReadProcessMemory(process,
                                  peb.ProcessParameters,
                                  &pebUpp,
                                  sizeof(RTL_USER_PROCESS_PARAMETERS),
                                  &dwBytesRead))
            {
                if (pebUpp.CommandLine.Length > 0)
                {
                    WCHAR *commandLine = (WCHAR *)malloc(pebUpp.CommandLine.Length);
                    if (commandLine != NULL)
                    {
                        if (ReadProcessMemory(process, pebUpp.CommandLine.Buffer,
                                              commandLine, pebUpp.CommandLine.Length, &dwBytesRead))
                        {
                            if (wcsstr(commandLine, excludeParam) == NULL)
                            {
                                WcaLog(LOGMSG_STANDARD, "Terminate process : %ls", commandLine);
                                TerminateProcess(process, 0);
                                processClosed = true;
                            }
                        }
                        free(commandLine);
                    }
                }
            }
        }
    }
    return processClosed;
}

// Terminate processes that do not have parameter [excludeParam]
// Note. This function relies on "NtQueryInformationProcess",
//       which may not be found.
//       Then all processes of [processName] will be terminated.
bool TerminateProcessesByNameW(LPCWSTR processName, LPCWSTR excludeParam)
{
    HMODULE hntdll = GetModuleHandleW(L"ntdll.dll");
    if (hntdll == NULL)
    {
        WcaLog(LOGMSG_STANDARD, "Failed to load ntdll.");
    }

    pfnNtQueryInformationProcess NtQueryInformationProcess = NULL;
    if (hntdll != NULL)
    {
        NtQueryInformationProcess = (pfnNtQueryInformationProcess)GetProcAddress(
            hntdll, "NtQueryInformationProcess");
    }
    if (NtQueryInformationProcess == NULL)
    {
        WcaLog(LOGMSG_STANDARD, "Failed to get address of NtQueryInformationProcess.");
    }

    bool processClosed = false;
    // Create a snapshot of the current system processes
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W processEntry;
        processEntry.dwSize = sizeof(PROCESSENTRY32W);
        if (Process32FirstW(snapshot, &processEntry))
        {
            do
            {
                if (lstrcmpW(processName, processEntry.szExeFile) == 0)
                {
                    HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processEntry.th32ProcessID);
                    if (process != NULL)
                    {
                        if (NtQueryInformationProcess == NULL)
                        {
                            WcaLog(LOGMSG_STANDARD, "Terminate process : %ls, while NtQueryInformationProcess is NULL", processName);
                            TerminateProcess(process, 0);
                            processClosed = true;
                        }
                        else
                        {
                            processClosed = TerminateProcessIfNotContainsParam(
                                NtQueryInformationProcess,
                                process,
                                excludeParam);
                        }
                        CloseHandle(process);
                    }
                }
            } while (Process32NextW(snapshot, &processEntry));
        }
        CloseHandle(snapshot);
    }
    if (hntdll != NULL)
    {
        CloseHandle(hntdll);
    }
    return processClosed;
}

UINT __stdcall TerminateProcesses(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    int nResult = 0;
    wchar_t szProcess[256] = {0};
    DWORD cchProcess = sizeof(szProcess) / sizeof(szProcess[0]);

    hr = WcaInitialize(hInstall, "TerminateProcesses");
    ExitOnFailure(hr, "Failed to initialize");

    MsiGetPropertyW(hInstall, L"TerminateProcesses", szProcess, &cchProcess);

    WcaLog(LOGMSG_STANDARD, "Try terminate processes : %ls", szProcess);
    TerminateProcessesByNameW(szProcess, L"--install");

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

// No use for now, it can be refer as an example of ShellExecuteW.
void AddFirewallRuleCmdline(LPWSTR exeName, LPWSTR exeFile, LPCWSTR dir)
{
    HRESULT hr = S_OK;
    HINSTANCE hi = 0;
    WCHAR cmdline[1024] = { 0, };
    WCHAR rulename[500] = { 0, };

    StringCchPrintfW(rulename, sizeof(rulename) / sizeof(rulename[0]), L"%ls Service", exeName);
    if (hr < 0) {
        WcaLog(LOGMSG_STANDARD, "Failed to make rulename: %ls", exeName);
        return;
    }

    StringCchPrintfW(cmdline, sizeof(cmdline) / sizeof(cmdline[0]), L"advfirewall firewall add rule name=\"%ls\" dir=%ls action=allow program=\"%ls\" enable=yes", rulename, dir, exeFile);
    if (hr < 0) {
        WcaLog(LOGMSG_STANDARD, "Failed to make cmdline: %ls", exeName);
        return;
    }

    hi = ShellExecuteW(NULL, L"open", L"netsh", cmdline, NULL, SW_HIDE);
    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew
    if ((int)hi <= 32) {
        WcaLog(LOGMSG_STANDARD, "Failed to change firewall rule : %d, last error: %d", (int)hi, GetLastError());
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Firewall rule \"%ls\" (%ls) is added", rulename, dir);
    }
}

// No use for now, it can be refer as an example of ShellExecuteW.
void RemoveFirewallRuleCmdline(LPWSTR exeName)
{
    HRESULT hr = S_OK;
    HINSTANCE hi = 0;
    WCHAR cmdline[1024] = { 0, };
    WCHAR rulename[500] = { 0, };

    StringCchPrintfW(rulename, sizeof(rulename) / sizeof(rulename[0]), L"%ls Service", exeName);
    if (hr < 0) {
        WcaLog(LOGMSG_STANDARD, "Failed to make rulename: %ls", exeName);
        return;
    }

    StringCchPrintfW(cmdline, sizeof(cmdline) / sizeof(cmdline[0]), L"advfirewall firewall delete rule name=\"%ls\"", rulename);
    if (hr < 0) {
        WcaLog(LOGMSG_STANDARD, "Failed to make cmdline: %ls", exeName);
        return;
    }

    hi = ShellExecuteW(NULL, L"open", L"netsh", cmdline, NULL, SW_HIDE);
    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew
    if ((int)hi <= 32) {
        WcaLog(LOGMSG_STANDARD, "Failed to change firewall rule \"%ls\" : %d, last error: %d", rulename, (int)hi, GetLastError());
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Firewall rule \"%ls\" is removed", rulename);
    }
}

UINT __stdcall AddFirewallRules(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    int nResult = 0;
    LPWSTR exeFile = NULL;
    LPWSTR exeName = NULL;
    WCHAR exeNameNoExt[500] = { 0, };
    LPWSTR pwz = NULL;
    LPWSTR pwzData = NULL;
    size_t szNameLen = 0;

    hr = WcaInitialize(hInstall, "AddFirewallRules");
    ExitOnFailure(hr, "Failed to initialize");

    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");

    pwz = pwzData;
    hr = WcaReadStringFromCaData(&pwz, &exeFile);
    ExitOnFailure(hr, "failed to read database key from custom action data: %ls", pwz);
    WcaLog(LOGMSG_STANDARD, "Try add firewall exceptions for file : %ls", exeFile);

    exeName = PathFindFileNameW(exeFile + 1);
    hr = StringCchPrintfW(exeNameNoExt, 500, exeName);
    ExitOnFailure(hr, "Failed to copy exe name: %ls", exeName);
    szNameLen = wcslen(exeNameNoExt);
    if (szNameLen >= 4 && wcscmp(exeNameNoExt + szNameLen - 4, L".exe") == 0) {
        exeNameNoExt[szNameLen - 4] = L'\0';
    }

    //if (exeFile[0] == L'1') {
    //    AddFirewallRuleCmdline(exeNameNoExt, exeFile, L"in");
    //    AddFirewallRuleCmdline(exeNameNoExt, exeFile, L"out");
    //}
    //else {
    //    RemoveFirewallRuleCmdline(exeNameNoExt);
    //}

    AddFirewallRule(exeFile[0] == L'1', exeNameNoExt, exeFile + 1);

LExit:
    if (pwzData) {
        ReleaseStr(pwzData);
    }

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall SetPropertyIsServiceRunning(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    wchar_t szAppName[500] = { 0 };
    DWORD cchAppName = sizeof(szAppName) / sizeof(szAppName[0]);
    wchar_t szPropertyName[500] = { 0 };
    DWORD cchPropertyName = sizeof(szPropertyName) / sizeof(szPropertyName[0]);
    bool isRunning = false;

    hr = WcaInitialize(hInstall, "SetPropertyIsServiceRunning");
    ExitOnFailure(hr, "Failed to initialize");

    MsiGetPropertyW(hInstall, L"AppName", szAppName, &cchAppName);
    WcaLog(LOGMSG_STANDARD, "Try query service of : \"%ls\"", szAppName);

    MsiGetPropertyW(hInstall, L"PropertyName", szPropertyName, &cchPropertyName);
    WcaLog(LOGMSG_STANDARD, "Try set is service running, property name : \"%ls\"", szPropertyName);

    isRunning = IsServiceRunningW(szAppName);
    MsiSetPropertyW(hInstall, szPropertyName, isRunning ? L"'N'" : L"'Y'");

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall CreateStartService(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    LPWSTR svcParams = NULL;
    LPWSTR pwz = NULL;
    LPWSTR pwzData = NULL;
    LPWSTR svcName = NULL;
    LPWSTR svcBinary = NULL;
    wchar_t szSvcDisplayName[500] = { 0 };
    DWORD cchSvcDisplayName = sizeof(szSvcDisplayName) / sizeof(szSvcDisplayName[0]);

    hr = WcaInitialize(hInstall, "CreateStartService");
    ExitOnFailure(hr, "Failed to initialize");

    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");

    pwz = pwzData;
    hr = WcaReadStringFromCaData(&pwz, &svcParams);
    ExitOnFailure(hr, "failed to read database key from custom action data: %ls", pwz);

    WcaLog(LOGMSG_STANDARD, "Try create start service : %ls", svcParams);

    svcName = svcParams;
    svcBinary = wcschr(svcParams, L';');
    if (svcBinary == NULL) {
        WcaLog(LOGMSG_STANDARD, "Failed to find binary : %ls", svcParams);
        goto LExit;
    }
    svcBinary[0] = L'\0';
    svcBinary += 1;

    hr = StringCchPrintfW(szSvcDisplayName, cchSvcDisplayName, L"%ls Service", svcName);
    ExitOnFailure(hr, "Failed to compose a resource identifier string");
    if (MyCreateServiceW(svcName, szSvcDisplayName, svcBinary)) {
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is created.", svcName);
        if (MyStartServiceW(svcName)) {
            WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is started.", svcName);
        }
        else {
            WcaLog(LOGMSG_STANDARD, "Failed to start service: \"%ls\"", svcName);
        }
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Failed to create service: \"%ls\"", svcName);
    }

LExit:
    if (pwzData) {
        ReleaseStr(pwzData);
    }

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall TryStopDeleteService(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    int nResult = 0;
    LPWSTR svcName = NULL;
    LPWSTR pwz = NULL;
    LPWSTR pwzData = NULL;
    wchar_t szExeFile[500] = { 0 };
    DWORD cchExeFile = sizeof(szExeFile) / sizeof(szExeFile[0]);

    hr = WcaInitialize(hInstall, "TryStopDeleteService");
    ExitOnFailure(hr, "Failed to initialize");

    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");

    pwz = pwzData;
    hr = WcaReadStringFromCaData(&pwz, &svcName);
    ExitOnFailure(hr, "failed to read database key from custom action data: %ls", pwz);
    WcaLog(LOGMSG_STANDARD, "Try stop and delete service : %ls", svcName);

    if (MyStopServiceW(svcName)) {
        for (int i = 0; i < 10; i++) {
            if (IsServiceRunningW(svcName)) {
                Sleep(100);
            }
            else {
                break;
            }
        }
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is stopped", svcName);
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Failed to stop service: \"%ls\"", svcName);
    }

    if (MyDeleteServiceW(svcName)) {
        WcaLog(LOGMSG_STANDARD, "Service \"%ls\" is deleted", svcName);
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Failed to delete service: \"%ls\"", svcName);
    }

    // It's really strange that we need sleep here.
    // But the upgrading may be stucked at "copying new files" because the file is in using.
    // Steps to reproduce: Install -> stop service in tray --> start service -> upgrade
    // Sleep(300);

    // Or we can terminate the process
    hr = StringCchPrintfW(szExeFile, cchExeFile, L"%ls.exe", svcName);
    ExitOnFailure(hr, "Failed to compose a resource identifier string");
    TerminateProcessesByNameW(szExeFile, L"--not-in-use");

LExit:
    if (pwzData) {
        ReleaseStr(pwzData);
    }

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall TryDeleteStartupShortcut(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    wchar_t szShortcut[500] = { 0 };
    DWORD cchShortcut = sizeof(szShortcut) / sizeof(szShortcut[0]);
    wchar_t szStartupDir[500] = { 0 };
    DWORD cchStartupDir = sizeof(szStartupDir) / sizeof(szStartupDir[0]);
    WCHAR pwszTemp[1024] = L"";

    hr = WcaInitialize(hInstall, "DeleteStartupShortcut");
    ExitOnFailure(hr, "Failed to initialize");

    MsiGetPropertyW(hInstall, L"StartupFolder", szStartupDir, &cchStartupDir);

    MsiGetPropertyW(hInstall, L"ShortcutName", szShortcut, &cchShortcut);
    WcaLog(LOGMSG_STANDARD, "Try delete startup shortcut of : \"%ls\"", szShortcut);

    hr = StringCchPrintfW(pwszTemp, 1024, L"%ls%ls.lnk", szStartupDir, szShortcut);
    ExitOnFailure(hr, "Failed to compose a resource identifier string");

    if (DeleteFileW(pwszTemp)) {
        WcaLog(LOGMSG_STANDARD, "Failed to delete startup shortcut of : \"%ls\"", pwszTemp);
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Startup shortcut is deleted : \"%ls\"", pwszTemp);
    }

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall SetPropertyFromConfig(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    wchar_t szConfigFile[1024] = { 0 };
    DWORD cchConfigFile = sizeof(szConfigFile) / sizeof(szConfigFile[0]);
    wchar_t szConfigKey[500] = { 0 };
    DWORD cchConfigKey = sizeof(szConfigKey) / sizeof(szConfigKey[0]);
    wchar_t szPropertyName[500] = { 0 };
    DWORD cchPropertyName = sizeof(szPropertyName) / sizeof(szPropertyName[0]);
    std::wstring configValue;

    hr = WcaInitialize(hInstall, "SetPropertyFromConfig");
    ExitOnFailure(hr, "Failed to initialize");

    MsiGetPropertyW(hInstall, L"ConfigFile", szConfigFile, &cchConfigFile);
    WcaLog(LOGMSG_STANDARD, "Try read config file of : \"%ls\"", szConfigFile);

    MsiGetPropertyW(hInstall, L"ConfigKey", szConfigKey, &cchConfigKey);
    WcaLog(LOGMSG_STANDARD, "Try read configuration, config key : \"%ls\"", szConfigKey);

    MsiGetPropertyW(hInstall, L"PropertyName", szPropertyName, &cchPropertyName);
    WcaLog(LOGMSG_STANDARD, "Try read configuration, property name : \"%ls\"", szPropertyName);

    configValue = ReadConfig(szConfigFile, szConfigKey);
    MsiSetPropertyW(hInstall, szPropertyName, configValue.c_str());

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall AddRegSoftwareSASGeneration(__in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

     LSTATUS result = 0;
     HKEY hKey;
     LPCWSTR subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System";
     LPCWSTR valueName = L"SoftwareSASGeneration";
     DWORD valueType = REG_DWORD;
     DWORD valueData = 1;
     DWORD valueDataSize = sizeof(DWORD);

    HINSTANCE hi = 0;

    hr = WcaInitialize(hInstall, "AddRegSoftwareSASGeneration");
    ExitOnFailure(hr, "Failed to initialize");

    hi = ShellExecuteW(NULL, L"open", L"reg", L" add HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System /f /v SoftwareSASGeneration /t REG_DWORD /d 1", NULL, SW_HIDE);
    // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew
    if ((int)hi <= 32) {
        WcaLog(LOGMSG_STANDARD, "Failed to add registry name \"%ls\", %d, %d", valueName, (int)hi, GetLastError());
    }
    else {
        WcaLog(LOGMSG_STANDARD, "Registry name \"%ls\" is added", valueName);
    }

    // Why RegSetValueExW always return 998?
    // 
    result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKey, 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (result != ERROR_SUCCESS) {
        WcaLog(LOGMSG_STANDARD, "Failed to create or open registry key: %d", result);
        goto LExit;
    }

    result = RegSetValueExW(hKey, valueName, 0, valueType, reinterpret_cast<const BYTE*>(valueData), valueDataSize);
    if (result != ERROR_SUCCESS) {
        WcaLog(LOGMSG_STANDARD, "Failed to set registry value: %d", result);
        RegCloseKey(hKey);
        goto LExit;
    }

    WcaLog(LOGMSG_STANDARD, "Registry value has been successfully set.");
    RegCloseKey(hKey);

LExit:
    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}

UINT __stdcall RemoveAmyuniIdd(
    __in MSIHANDLE hInstall)
{
    HRESULT hr = S_OK;
    DWORD er = ERROR_SUCCESS;

    int nResult = 0;
    LPWSTR installFolder = NULL;
    LPWSTR pwz = NULL;
    LPWSTR pwzData = NULL;

    WCHAR workDir[1024] = L"";
    DWORD fileAttributes = 0;
    HINSTANCE hi = 0;

    USHORT processMachine = 0;
    USHORT nativeMachine = 0;
    BOOL isWow64Res = FALSE;
    LPCWSTR exe = NULL;

    hr = WcaInitialize(hInstall, "RemoveAmyuniIdd");
    ExitOnFailure(hr, "Failed to initialize");

    hr = WcaGetProperty(L"CustomActionData", &pwzData);
    ExitOnFailure(hr, "failed to get CustomActionData");

    pwz = pwzData;
    hr = WcaReadStringFromCaData(&pwz, &installFolder);
    ExitOnFailure(hr, "failed to read database key from custom action data: %ls", pwz);

    hr = StringCchPrintfW(workDir, 1024, L"%lsusbmmidd_v2", installFolder);
    ExitOnFailure(hr, "Failed to compose a resource identifier string");
    fileAttributes = GetFileAttributesW(workDir);
    if (fileAttributes == INVALID_FILE_ATTRIBUTES) {
        WcaLog(LOGMSG_STANDARD, "Amyuni idd dir \"%ls\" is out found, %d", workDir, fileAttributes);
        goto LExit;
    }

    isWow64Res = IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine);
    if (isWow64Res == TRUE) {
        if (nativeMachine == IMAGE_FILE_MACHINE_AMD64) {
            exe = L"deviceinstaller64.exe";
        } else {
            exe = L"deviceinstaller.exe";
        }
        WcaLog(LOGMSG_STANDARD, "Remove amyuni idd %ls in %ls", exe, workDir);
        hi = ShellExecuteW(NULL, L"open", exe, L"remove usbmmidd", workDir, SW_HIDE);
        // https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shellexecutew
        if ((int)hi <= 32) {
            WcaLog(LOGMSG_STANDARD, "Failed to remove amyuni idd : %d, last error: %d", (int)hi, GetLastError());
        }
        else {
            WcaLog(LOGMSG_STANDARD, "Amyuni idd is removed");
        }
    } else {
        WcaLog(LOGMSG_STANDARD, "Failed to call IsWow64Process2(): %d", GetLastError());
    }

LExit:
    ReleaseStr(installFolder);

    er = SUCCEEDED(hr) ? ERROR_SUCCESS : ERROR_INSTALL_FAILURE;
    return WcaFinalize(er);
}
