#include "SupportPackageManager.h"
#include "uevr/API.hpp"
#include <shlobj.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")

namespace {
    bool IsFile(const std::wstring& path) {
        const DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    }
    std::wstring Quote(const std::wstring& value) { return L"\"" + value + L"\""; }
}

SupportPackageManager::~SupportPackageManager() {
    // The independent helper may finish after game exit. Never kill it at detach.
    if (process) CloseHandle(process);
}

void SupportPackageManager::Initialize(const std::string& profile) {
    if (!profilePath.empty()) return;
    const int size = MultiByteToWideChar(CP_UTF8, 0, profile.c_str(), -1, nullptr, 0);
    if (size <= 1) return;
    std::wstring converted(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, profile.c_str(), -1, converted.data(), size);
    converted.resize(static_cast<size_t>(size - 1));
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return;
    documentsPath = docs; CoTaskMemFree(docs);
    profilePath = converted;
    supportHome = documentsPath + L"\\San Andreas VR";
}

void SupportPackageManager::Start() {
    std::wstring helperHome = profilePath + L"\\Support";
    if (!IsFile(helperHome + L"\\SAVR-SupportCore.ps1")) helperHome = supportHome;
    const std::wstring script = helperHome + L"\\_INTERNAL - SAVR Support Tool Script.ps1";
    // An old script would ignore -Action and open its GUI. Require the new companion.
    if (profilePath.empty() || !IsFile(script) || !IsFile(helperHome + L"\\SAVR-SupportCore.ps1")) {
        state = SupportPackageState::ToolMissing;
        uevr::API::get()->log_warn("[SupportPackage] helper missing; update installed support tool");
        return;
    }
    wchar_t system[MAX_PATH]{}, exe[32768]{};
    const UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
    const DWORD exeLength = GetModuleFileNameW(nullptr, exe, 32768);
    if (!systemLength || systemLength >= MAX_PATH || !exeLength || exeLength >= 32768) {
        state = SupportPackageState::Failed; return;
    }
    const std::wstring powershell = std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    requestId = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    resultPath = profilePath + L"\\SAVR_support_" + requestId + L".ini";
    std::wstring args = Quote(powershell) + L" -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File "
        + Quote(script) + L" -Action Collect -RequestId " + requestId + L" -ProfilePath " + Quote(profilePath)
        + L" -DocumentsPath " + Quote(documentsPath) + L" -GameExe " + Quote(exe);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(powershell.c_str(), args.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, helperHome.c_str(), &startup, &child)) {
        state = SupportPackageState::Failed;
        uevr::API::get()->log_warn("[SupportPackage] helper launch failed error=%lu", GetLastError());
        return;
    }
    CloseHandle(child.hThread); process = child.hProcess;
    state = SupportPackageState::Creating; nextPoll = GetTickCount64() + 500;
    uevr::API::get()->log_info("[SupportPackage] background collection started; no desktop window requested");
}

bool SupportPackageManager::Update() {
    const auto before = state.load();
    const int action = request.exchange(0);
    if (action == 1 && !process) Start();
    if (action == 2 && !supportHome.empty()) {
        const std::wstring folder = supportHome + L"\\Support Packages";
        const DWORD attr = GetFileAttributesW(folder.c_str());
        // Explicit user action only. Completion never opens Explorer or changes VR state.
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            if (!process) state = result > 32 ? SupportPackageState::FolderOpened : SupportPackageState::Failed;
            uevr::API::get()->log_info("[SupportPackage] user requested desktop folder; result=%lld", static_cast<long long>(result));
        } else if (!process) state = SupportPackageState::Failed;
    }
    if (process && GetTickCount64() >= nextPoll) {
        nextPoll = GetTickCount64() + 500;
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
            DWORD exitCode = 1; GetExitCodeProcess(process, &exitCode);
            CloseHandle(process); process = nullptr;
            wchar_t reportedId[96]{}, status[32]{}, leaf[160]{};
            GetPrivateProfileStringW(L"Support", L"RequestId", L"", reportedId, 96, resultPath.c_str());
            GetPrivateProfileStringW(L"Support", L"State", L"", status, 32, resultPath.c_str());
            GetPrivateProfileStringW(L"Support", L"PackageName", L"", leaf, 160, resultPath.c_str());
            const std::wstring name = leaf;
            const bool safeName = name.rfind(L"SAVR-Support-", 0) == 0 && name.size() > 4
                && name.substr(name.size()-4) == L".zip" && name.find_first_of(L"\\/:\"") == std::wstring::npos;
            const bool saved = exitCode == 0 && requestId == reportedId && wcscmp(status, L"saved") == 0
                && safeName && IsFile(supportHome + L"\\Support Packages\\" + name);
            state = saved ? SupportPackageState::Saved : SupportPackageState::Failed;
            DeleteFileW(resultPath.c_str());
            uevr::API::get()->log_info("[SupportPackage] finished saved=%s exit=%lu; location=Documents/San Andreas VR/Support Packages",
                saved ? "true" : "false", exitCode);
        }
    }
    return before != state.load();
}
