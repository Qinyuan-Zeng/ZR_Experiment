#include <windows.h>
#include <iostream>
#include "gts.h"

// Reference the SDK symbol to verify the import library and DLL linkage.
// This program does not call any controller API.
auto volatile sdk_open_entry = &GTN_Open;

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    HMODULE sdk = GetModuleHandleW(L"gts.dll");
    if (!sdk || !sdk_open_entry) {
        std::cerr << "SDK load check failed.\n";
        return 1;
    }
    wchar_t path[32768] = {};
    if (!GetModuleFileNameW(sdk, path, 32768)) {
        std::cerr << "Cannot read SDK module path.\n";
        return 2;
    }
    std::cout << "PASS: C++ build, x64 gts.lib linkage and gts.dll loading.\n";
    std::cout << "SDK module path read successfully.\n";
    std::cout << "Offline environment check only. No controller API was called.\n";
    return 0;
}

