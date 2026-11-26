// Software-only tests. This executable supplies fake API functions and never
// links gts.lib, loads gts.dll or communicates with hardware.
#define main hardware_main_not_called
#include "../main.cpp"
#undef main
#include <cassert>

static TSwitchLoopParameter fakeSwitch = {};
static long fakeStatus = 0;
static short fakeMode = 2;
static double fakeTarget = 0;
static int switchWrites = 0, forceWrites = 0;
static bool failFirstSet = false, corruptRestore = false, failStatus = false;

GT_API GTN_OpenCard(short, void*, char*) { return -99; }
GT_API GTN_NetInit(short, void*, short, long*) { return -99; }
GT_API GTN_Close() { return 0; }
GT_API GTN_GetVersion(short, char**) { return -99; }
GT_API GTN_GetSts(short, short, long* s, short, unsigned long*) {
    *s = fakeStatus; return failStatus ? -1 : 0;
}
GT_API GTN_GetControlLoopMode(short, short, short, short* m) { *m = fakeMode; return 0; }
GT_API GTN_GetSwitchLoopParameter(short, short, TSwitchLoopParameter* p) { *p = fakeSwitch; return 0; }
GT_API GTN_SetSwitchLoopParameter(short, short, TSwitchLoopParameter* p) {
    ++switchWrites;
    assert(p->enable == 0);
    fakeSwitch = *p;
    if (corruptRestore && switchWrites == 2) fakeSwitch.threshold = 99;
    return failFirstSet && switchWrites == 1 ? -1 : 0;
}
GT_API GTN_GetForceValue(short, short, short, double* p) { *p = fakeTarget; return 0; }
GT_API GTN_SetForceValue(short, short, short, double v) {
    assert(v == 0); ++forceWrites; fakeTarget = v; return 0;
}
GT_API GTN_GetForceControlParameter(short, short, TForceControlPrm* p) { *p = {}; return 0; }

static void reset() {
    fakeSwitch = {}; fakeStatus = 0; fakeMode = 2; fakeTarget = 0;
    switchWrites = forceWrites = 0;
    failFirstSet = corruptRestore = failStatus = false;
}

int main() {
    reset();
    assert(runWriteChecks(2, 1, 1) == 0);
    assert(switchWrites == 2 && forceWrites == 1 && fakeSwitch.threshold == 0);
    reset(); fakeStatus = 0x200;
    assert(runWriteChecks(2, 1, 1) == 2 && switchWrites == 0 && forceWrites == 0);
    reset(); fakeMode = 4;
    assert(runWriteChecks(2, 1, 1) == 2 && switchWrites == 0);
    reset(); fakeSwitch.enable = 1;
    assert(runWriteChecks(2, 1, 1) == 2 && switchWrites == 0);
    reset(); failStatus = true;
    assert(runWriteChecks(2, 1, 1) == 2 && switchWrites == 0);
    reset(); failFirstSet = true;
    assert(runWriteChecks(2, 1, 1) == 1);
    assert(switchWrites == 2 && fakeSwitch.threshold == 0 && forceWrites == 0);
    reset(); corruptRestore = true;
    assert(runWriteChecks(2, 1, 1) == 1 && forceWrites == 0);
    reset(); fakeTarget = 123;
    assert(runWriteChecks(2, 1, 1) == 2 && forceWrites == 0 && fakeTarget == 123);
    reset(); fakeSwitch.threshold = 1;
    assert(runWriteChecks(2, 1, 1) == 0 && fakeSwitch.threshold == 1);
    std::puts("OFFLINE CHECKS PASSED: 9 scenarios; no hardware library linked.");
    return 0;
}
