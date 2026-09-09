#pragma once
#include <atomic>
#include <string>
#include <windows.h>

enum class SupportPackageState : unsigned { Ready, Creating, Saved, Failed, ToolMissing, FolderOpened };

// Owns only the external support helper. No camera, input, game-memory or UObject writes.
class SupportPackageManager {
public:
    ~SupportPackageManager();
    void Initialize(const std::string& profile);
    void RequestCollect() { request.store(1); }
    void RequestOpenFolder() { request.store(2); }
    bool Update(); // Engine-thread only; returns true when displayed status changes.
    SupportPackageState GetState() const { return state.load(); }
private:
    void Start();
    std::atomic<int> request{0};
    std::atomic<SupportPackageState> state{SupportPackageState::Ready};
    std::wstring profilePath, documentsPath, supportHome, requestId, resultPath;
    HANDLE process = nullptr;
    ULONGLONG nextPoll = 0;
};
