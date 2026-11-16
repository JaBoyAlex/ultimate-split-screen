#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

namespace {
constexpr wchar_t game_name[] = L"LEGOStarWarsSaga.exe";
constexpr wchar_t dll_name[] = L"TCS-Split-Prototype.dll";
constexpr wchar_t release_name[] = L"Ultimate Split-Screen";
// Steam app 32440, build 1017673. The DLL uses addresses from this executable.
constexpr unsigned char supported_sha256[] = {
    0x08, 0x56, 0x88, 0xe7, 0x35, 0x61, 0xc6, 0x1f, 0x46, 0x19, 0xfc, 0xf1, 0xf3, 0x00, 0x6c, 0x56,
    0x42, 0x5c, 0xa3, 0x65, 0xe8, 0x0e, 0xa5, 0xfc, 0x01, 0x75, 0xba, 0xb3, 0x34, 0xce, 0xc6, 0x71,
};
constexpr LONGLONG supported_size = 5472256;
constexpr wchar_t steam_launch_url[] = L"steam://rungameid/32440";
constexpr ULONGLONG startup_timeout_ms = 120'000;
// Read these only after checking the executable hash.
constexpr uintptr_t game_window_address = 0x924864;
constexpr uintptr_t game_device_address = 0x29765cc;

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

int fail(const wchar_t* message) {
    std::fwprintf(stderr, L"%ls\n", message);
    DWORD console_process;
    if (GetConsoleProcessList(&console_process, 1) == 1)
        MessageBoxW(nullptr, message, release_name, MB_OK | MB_ICONERROR);
    return 1;
}

bool supported_executable(HANDLE game) {
    wchar_t path[32768];
    DWORD path_size = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    if (!QueryFullProcessImageNameW(game, 0, path, &path_size)) return false;
    Handle file{CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    LARGE_INTEGER size{};
    if (file.value == INVALID_HANDLE_VALUE || !GetFileSizeEx(file.value, &size)
        || size.QuadPart != supported_size) return false;
    Handle mapping{CreateFileMappingW(file.value, nullptr, PAGE_READONLY, 0, 0, nullptr)};
    if (!mapping.value) return false;
    auto* bytes = static_cast<unsigned char*>(MapViewOfFile(mapping.value, FILE_MAP_READ, 0, 0, 0));
    if (!bytes) return false;

    BCRYPT_ALG_HANDLE sha256 = nullptr;
    unsigned char actual[sizeof(supported_sha256)];
    bool matches = false;
    if (BCryptOpenAlgorithmProvider(&sha256, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0) {
        const auto status = BCryptHash(sha256, nullptr, 0, bytes, static_cast<ULONG>(size.QuadPart),
                                       actual, static_cast<ULONG>(sizeof(actual)));
        matches = status >= 0 && std::memcmp(actual, supported_sha256, sizeof(actual)) == 0;
        BCryptCloseAlgorithmProvider(sha256, 0);
    }
    UnmapViewOfFile(bytes);
    return matches;
}

struct GameProcess {
    DWORD pid = 0;
    bool multiple = false;
};

GameProcess find_game() {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD found = 0;
    if (Process32FirstW(snapshot.value, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, game_name) != 0) continue;
            if (found) return {0, true};
            found = entry.th32ProcessID;
        } while (Process32NextW(snapshot.value, &entry));
    }
    return {found, false};
}

GameProcess wait_for_game(ULONGLONG deadline) {
    do {
        const auto game = find_game();
        if (game.pid || game.multiple) return game;
        Sleep(250);
    } while (GetTickCount64() < deadline);
    return {};
}

bool wait_for_renderer(HANDLE game, DWORD pid, ULONGLONG deadline) {
    do {
        if (WaitForSingleObject(game, 0) != WAIT_TIMEOUT) return false;

        HWND window = nullptr;
        uintptr_t device = 0;
        DWORD window_pid = 0;
        if (ReadProcessMemory(game, reinterpret_cast<const void*>(game_window_address),
                              &window, sizeof(window), nullptr)
            && ReadProcessMemory(game, reinterpret_cast<const void*>(game_device_address),
                                 &device, sizeof(device), nullptr)
            && device && window && GetWindowThreadProcessId(window, &window_pid)
            && window_pid == pid && IsWindowVisible(window)
            && SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, nullptr)) {
            return true;
        }
        Sleep(250);
    } while (GetTickCount64() < deadline);
    return false;
}

uintptr_t module_address(DWORD pid, const wchar_t* name) {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid)};
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot.value, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, name) == 0) return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        } while (Module32NextW(snapshot.value, &entry));
    }
    return 0;
}

bool remote_call(HANDLE game, uintptr_t address, void* argument, DWORD& result) {
    Handle thread{CreateRemoteThread(game, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(address),
                                     argument, 0, nullptr)};
    return thread.value && WaitForSingleObject(thread.value, INFINITE) == WAIT_OBJECT_0
        && GetExitCodeThread(thread.value, &result);
}
}

int wmain(int argc, wchar_t** argv) {
    const bool launch = argc == 2 && std::wcscmp(argv[1], L"--launch") == 0;
    if (argc != 1 && !launch) return fail(L"Usage: Enable-Split.exe [--launch]");

    Handle launch_lock{CreateMutexW(nullptr, FALSE, L"Local\\UltimateSplitScreenLauncher")};
    if (launch_lock.value && GetLastError() == ERROR_ALREADY_EXISTS) {
        std::puts("Ultimate Split-Screen is already starting.");
        return 0;
    }

    const ULONGLONG deadline = GetTickCount64() + startup_timeout_ms;
    auto target = find_game();
    if (launch && !target.pid && !target.multiple) {
        std::puts("Starting LEGO Star Wars: The Complete Saga through Steam...");
        const auto launched = ShellExecuteW(nullptr, L"open", steam_launch_url, nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(launched) <= 32)
            return fail(L"Could not open the Steam launch link. Install Steam and the game, then try again.");
        target = wait_for_game(deadline);
    }
    if (target.multiple) return fail(L"More than one copy of the game is open. Leave one running and try again.");
    if (!target.pid) {
        return fail(launch ? L"Timed out waiting for the game. Check Steam, then try again."
                           : L"Start the game through Steam or use Launch Ultimate Split-Screen.bat.");
    }
    const DWORD pid = target.pid;
    if (module_address(pid, dll_name)) {
        std::puts("The mod is already loaded. Restart the game before changing mod versions.");
        return 0;
    }
    Handle game{OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
                           | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid)};
    if (!game.value) return fail(L"Could not access the game. Run Steam and Enable-Split.exe as the same Windows user.");
    if (!supported_executable(game.value)) {
        return fail(L"This release requires the unmodified Steam PC executable from build 1017673.");
    }
    if (launch && !wait_for_renderer(game.value, pid, deadline)) {
        return fail(L"The game closed or its renderer did not become ready. Check the game, then try again.");
    }
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return fail(L"Extract the mod to a folder with a shorter path and try again.");
    std::wstring dll(path, wcsrchr(path, L'\\') + 1);
    dll += dll_name;
    HMODULE inspection = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!inspection) return fail(L"Keep TCS-Split-Prototype.dll next to Enable-Split.exe in the extracted folder.");
    const uintptr_t enable = reinterpret_cast<uintptr_t>(GetProcAddress(inspection, "EnableSplit"));
    const uintptr_t offset = enable - reinterpret_cast<uintptr_t>(inspection);
    FreeLibrary(inspection);
    if (!enable) return fail(L"The DLL is missing the mod entry point. Extract a fresh copy of the ZIP.");

    FARPROC loader = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner;
    if (!loader || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCWSTR>(loader), &owner)
        || !GetModuleFileNameW(owner, path, MAX_PATH)) return fail(L"Could not resolve the Windows DLL loader.");
    const uintptr_t remote_owner = module_address(pid, wcsrchr(path, L'\\') + 1);
    if (!remote_owner) return fail(L"Could not find the Windows DLL loader in the game. Wait for the title screen and retry.");
    const uintptr_t remote_loader = remote_owner + reinterpret_cast<uintptr_t>(loader) - reinterpret_cast<uintptr_t>(owner);
    const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote_path = VirtualAllocEx(game.value, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remote_path) return fail(L"Could not allocate memory in the game process.");
    DWORD loaded = 0, enabled = 0;
    const bool wrote = WriteProcessMemory(game.value, remote_path, dll.c_str(), bytes, nullptr);
    if (wrote) remote_call(game.value, remote_loader, remote_path, loaded);
    VirtualFreeEx(game.value, remote_path, 0, MEM_RELEASE);
    if (!loaded || !remote_call(game.value, loaded + offset, nullptr, enabled) || enabled != 1) {
        return fail(L"Could not enable split-screen. Restart the game and try again.\n"
                    L"See %TEMP%\\TCS-Split-Prototype.log for details.");
    }
    std::puts("Split-screen enabled. Join with Player 2 using the game's normal co-op control.");
    return 0;
}
