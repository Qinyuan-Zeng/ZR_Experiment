#pragma once
#include "gts.h"

// Supplement to the supplied vc_gts header, transcribed from the V1.01
// force-control document. Windows/MSVC default packing is required.
typedef struct {
    short controlType;
    short feedbackType;
    short feedbackSrcIndex;
    double feedbackForceK;
    double feedbackForceB;
    double controlforceK;
    double controlforceB;
    double outputLimit;
    short outputReverse;
    short calibrationMode;
} TForceControlPrm;

GT_API GTN_GetForceControlParameter(short core, short control,
                                    TForceControlPrm* pForceControlPrm);

static_assert(sizeof(short) == 2 && sizeof(long) == 4, "Windows ABI required");
static_assert(sizeof(TForceControlPrm) == 56, "Unexpected structure packing");
static_assert(sizeof(TSwitchLoopParameter) == 32, "Unexpected switch structure packing");
