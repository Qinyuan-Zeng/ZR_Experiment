#pragma once

#include "gts.h"
#include <cstddef>

// Watch declarations from the local vendor's gxn.h (friction_ff_compensation).
// The matching names are exported by this project's x64 gts.dll and gts.lib.
// Keep the vendor structs' Windows layout. The application uses gts exclusively.
struct TWatchInfo {
    short enable, run;
    unsigned long time, head, threshold;
    short interval, mode;
    unsigned short countBeforeEvent, countAfterEvent, varCount, eventCount;
};

struct TWatchVarInfo {
    unsigned long* pAddress;
    unsigned short length;
    short fraction;
    unsigned short format, hex, sign, bit;
};

static_assert(sizeof(TWatchInfo) == 28 && offsetof(TWatchInfo, varCount) == 24);
static_assert(sizeof(TWatchVarInfo) == 24 && offsetof(TWatchVarInfo, sign) == 16);
static_assert(sizeof(TWatchVar) == 6);

GT_API GTN_GetWatchGroup(short core, short* group);
GT_API GTN_SetWatchGroup(short core, short group);
GT_API GTN_GetWatchInfoEx(short core, short group, TWatchInfo* info);
GT_API GTN_ClearWatch(short core, short mode);
GT_API GTN_AddWatchVar(short core, const TWatchVar* var);
GT_API GTN_GetWatchVar(short core, short index, TWatchVar* var, TWatchVarInfo* info);
GT_API GTN_WatchOn(short core, short interval, short mode, unsigned short count);
GT_API GTN_WatchOff(short core);
GT_API GTN_ReadWatch(short core, short varIndex, double* buffer, unsigned long capacity,
                       unsigned long* count);
