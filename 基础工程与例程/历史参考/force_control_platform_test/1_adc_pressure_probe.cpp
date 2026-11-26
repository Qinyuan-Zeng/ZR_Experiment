#include <windows.h>
#include <mmsystem.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gxn.h"

#pragma comment(lib, "gxn.lib")
#pragma comment(lib, "winmm.lib")

#define CORE 2

static const char* NET_XML_FILE = "Googol.xml";
static const char* CSV_LOG_FILE = "adc_force_probe.csv";

// C2 pins 8 and 26 are analog input 1+ and 1-. The GVN channel number is 1-based.
static const short DEFAULT_ADC_CHANNEL = 1;

struct ProbeConfig {
    short channel = DEFAULT_ADC_CHANNEL;
    bool useAuxAdc = false;
    int sampleMs = 10;
    int durationMs = 30000;
    double zeroVoltage = 0.0;
    double forcePerVolt = 9.81;
    const char* csvPath = CSV_LOG_FILE;
};

static void printUsage(const char* exe) {
    printf("Usage: %s [options]\n", exe);
    printf("Options:\n");
    printf("  --channel N             ADC channel, 1-based. Default: 1 for C2-8/C2-26\n");
    printf("  --aux                   Use GTN_GetAuAdc/GTN_GetAuAdcValue instead of GTN_GetAdc\n");
    printf("  --sample-ms N           Sampling interval. Default: 10\n");
    printf("  --duration-ms N         Run duration. Default: 30000\n");
    printf("  --zero-v V              Zero-pressure voltage offset. Default: 0\n");
    printf("  --force-per-v K         force_n = (voltage - zero-v) * K. Default: 9.81\n");
    printf("  --csv PATH              Output CSV path. Default: adc_force_probe.csv\n");
}

static bool parseShort(const char* text, short* out) {
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value < 1 || value > 32767) {
        return false;
    }
    *out = (short)value;
    return true;
}

static bool parseInt(const char* text, int* out) {
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value <= 0 || value > 24 * 60 * 60 * 1000L) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool parseDouble(const char* text, double* out) {
    char* end = NULL;
    double value = strtod(text, &end);
    if (!end || *end != '\0' || !std::isfinite(value)) {
        return false;
    }
    *out = value;
    return true;
}

static bool parseArgs(int argc, char** argv, ProbeConfig* cfg) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            printUsage(argv[0]);
            return false;
        }
        if (strcmp(arg, "--aux") == 0) {
            cfg->useAuxAdc = true;
            continue;
        }

        if (i + 1 >= argc) {
            printf("missing value after %s\n", arg);
            return false;
        }
        const char* value = argv[++i];

        if (strcmp(arg, "--channel") == 0) {
            if (!parseShort(value, &cfg->channel)) return false;
        } else if (strcmp(arg, "--sample-ms") == 0) {
            if (!parseInt(value, &cfg->sampleMs)) return false;
        } else if (strcmp(arg, "--duration-ms") == 0) {
            if (!parseInt(value, &cfg->durationMs)) return false;
        } else if (strcmp(arg, "--zero-v") == 0) {
            if (!parseDouble(value, &cfg->zeroVoltage)) return false;
        } else if (strcmp(arg, "--force-per-v") == 0 || strcmp(arg, "--pressure-per-v") == 0) {
            if (!parseDouble(value, &cfg->forcePerVolt)) return false;
        } else if (strcmp(arg, "--csv") == 0) {
            cfg->csvPath = value;
        } else {
            printf("unknown option: %s\n", arg);
            return false;
        }
    }
    return true;
}

static short readAdc(const ProbeConfig& cfg,
                     double* voltage,
                     short* raw,
                     unsigned long* voltageClock,
                     unsigned long* rawClock,
                     short* rawRet) {
    if (cfg.useAuxAdc) {
        short ret = GTN_GetAuAdc(CORE, cfg.channel, voltage, 1, voltageClock);
        *rawRet = GTN_GetAuAdcValue(CORE, cfg.channel, raw, 1, rawClock);
        return ret;
    }

    short ret = GTN_GetAdc(CORE, cfg.channel, voltage, 1, voltageClock);
    *rawRet = GTN_GetAdcValue(CORE, cfg.channel, raw, 1, rawClock);
    return ret;
}

int main(int argc, char** argv) {
    ProbeConfig cfg;
    if (!parseArgs(argc, argv, &cfg)) {
        printUsage(argv[0]);
        return 2;
    }

    printf("ADC force probe\n");
    printf("XML: %s\n", NET_XML_FILE);
    printf("CORE=%d channel=%d mode=%s\n", CORE, cfg.channel, cfg.useAuxAdc ? "aux adc" : "axis adc");
    printf("force_n = (voltage - %.9g) * %.9g\n", cfg.zeroVoltage, cfg.forcePerVolt);
    printf("sample=%d ms duration=%d ms csv=%s\n", cfg.sampleMs, cfg.durationMs, cfg.csvPath);
    printf("This program only reads analog input; it does not enable or move any axis.\n");
    printf("Press Enter to start, or Ctrl+C to abort.\n");
    getchar();

    FILE* fp = NULL;
    fopen_s(&fp, cfg.csvPath, "w");
    if (!fp) {
        printf("cannot open csv: %s\n", cfg.csvPath);
        return 1;
    }

    fprintf(fp, "time_s,adc_mode,channel,voltage_v,raw_value,force_n,voltage_clock,raw_clock,voltage_ret,raw_ret\n");

    timeBeginPeriod(1);

    short ret = GTN_OpenCard();
    if (ret != 0) {
        printf("GTN_OpenCard failed: %d\n", ret);
        fclose(fp);
        timeEndPeriod(1);
        return 1;
    }

    long netStatus = 0;
    ret = GTN_NetInit(NET_INIT_MODE_XML_STRICT, const_cast<char*>(NET_XML_FILE), 60, &netStatus);
    if (ret != 0) {
        printf("GTN_NetInit failed: ret=%d status=%ld\n", ret, netStatus);
        GTN_Close();
        fclose(fp);
        timeEndPeriod(1);
        return 1;
    }

    const long startMs = (long)timeGetTime();
    long lastPrintMs = startMs;

    while (((long)timeGetTime() - startMs) <= cfg.durationMs) {
        double voltage = 0.0;
        short raw = 0;
        unsigned long voltageClock = 0;
        unsigned long rawClock = 0;
        short rawRet = 0;
        short voltageRet = readAdc(cfg, &voltage, &raw, &voltageClock, &rawClock, &rawRet);

        double forceN = (voltage - cfg.zeroVoltage) * cfg.forcePerVolt;
        double t = ((long)timeGetTime() - startMs) / 1000.0;

        fprintf(fp, "%.6f,%s,%d,%.9f,%d,%.9f,%lu,%lu,%d,%d\n",
                t,
                cfg.useAuxAdc ? "au_adc" : "adc",
                cfg.channel,
                voltage,
                raw,
                forceN,
                voltageClock,
                rawClock,
                voltageRet,
                rawRet);

        long nowMs = (long)timeGetTime();
        if (nowMs - lastPrintMs >= 500) {
            printf("t=%7.3f s  voltage=% .6f V  raw=%6d  force=% .6f N  ret=%d/%d\n",
                   t, voltage, raw, forceN, voltageRet, rawRet);
            lastPrintMs = nowMs;
        }

        Sleep(cfg.sampleMs);
    }

    fflush(fp);
    fclose(fp);
    GTN_Close();
    timeEndPeriod(1);

    printf("done. CSV: %s\n", cfg.csvPath);
    return 0;
}

