#include "stage1.hpp"

#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

fs::path menuScript()
{
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return {};
    for (auto dir = fs::path(executable).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        const auto script = dir / L"scripts" / L"a_formal.py";
        if (fs::is_regular_file(script)) return script;
        if (dir == dir.root_path()) break;
    }
    return {};
}

fs::path bundledPython()
{
    wchar_t profile[32768]{};
    const auto length = GetEnvironmentVariableW(L"USERPROFILE", profile, 32768);
    if (!length || length >= 32768) return {};
    const auto python = fs::path(profile) /
        L".cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe";
    return fs::is_regular_file(python) ? python : fs::path{};
}

int showMenu()
{
    const auto script = menuScript();
    if (script.empty()) {
        std::cerr << "Cannot find scripts/a_formal.py. Keep the executable inside the formal A project.\n";
        return 1;
    }
    const auto python = bundledPython();
    const std::wstring launcher = python.empty() ? L"py.exe" : python.wstring();
    std::wstring command = L"\"" + launcher + L"\"";
    if (python.empty()) command += L" -3";
    command += L" \"" + script.wstring() + L"\" --menu";
    std::vector<wchar_t> arguments(command.begin(), command.end());
    arguments.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(python.empty() ? nullptr : python.c_str(), arguments.data(),
                        nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
        std::cerr << "Cannot start Python. Use scripts/a_formal.py with the project Python interpreter. "
                  << "Windows error " << GetLastError() << '\n';
        return 1;
    }
    CloseHandle(process.hThread);
    const auto wait = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hProcess);
    return status == 0 ? 0 : 1;
}
}

int main(int argc, char** argv)
{
    if (argc > 1) return runExperiment("a_scan", argc, argv);
    int result = 1;
    try {
        result = showMenu();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    DWORD mode = 0;
    if (result && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) {
        std::cout << "Press Enter to close." << std::endl;
        std::string line;
        std::getline(std::cin, line);
    }
    return result;
}
