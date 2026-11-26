#include <windows.h>
#include "gts.h"
#include <iostream>

int main()
{
    const short opened = GTN_OpenCard(5, nullptr, nullptr); // PCIe
    std::cout << "GTN_OpenCard(5, nullptr, nullptr): " << opened << std::endl;
    if (opened != 0) return 1;

    TVersion firmware{};
    constexpr short core = 1;
    constexpr short cardFirmware = 1;
    const short read = GTN_GetVersionEx(core, cardFirmware, &firmware);
    std::cout << "GTN_GetVersionEx(core=1, type=1): " << read << std::endl;
    if (read == 0) {
        std::cout << "Card firmware: " << firmware.version << " ("
                  << firmware.year << '-' << firmware.month << '-' << firmware.day << ")\n";
    }
    const short closed = GTN_Close();
    std::cout << "GTN_Close: " << closed << std::endl;
    if (read != 0 || closed != 0) return 2;
    std::cout << "PASS: PC to PCIe card communication.\n";
    return 0;
}
