#pragma once

#define NOMINMAX
#include "watch_sdk.hpp"
#include <Windows.h>
#include <conio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace watch_capture {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Tick = std::uint32_t;
using Ns = std::int64_t;

constexpr short core = 2;
constexpr short axis = 1;
constexpr short group = WATCH_GROUP_TIMER;
constexpr short interval = 0;
constexpr unsigned short watchCount = 0;
constexpr unsigned long maxReadCapacity = 1000000;
const std::array<TWatchVar, 3> channels{{
    {WATCH_VAR_PRF_LOOP, 1, 1},
    {WATCH_VAR_ENC_POS, axis, 1},
    {WATCH_VAR_ATL_TORQUE, axis, 1},
}};
constexpr char csvHeader[] = "frame,card_tick_raw,encoder_position_raw,current_feedback_raw";
inline std::atomic_bool cancelled{false};

inline BOOL WINAPI onCtrl(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    cancelled.store(true);
    return TRUE;
}

inline bool stopRequested() {
    if (_kbhit()) {
        const int key = _getch();
        if (key == 'q' || key == 'Q') cancelled.store(true);
    }
    return cancelled.load();
}

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

inline double parseNumber(const std::string& value) {
    std::size_t end = 0;
    const double number = std::stod(value, &end);
    require(end == value.size() && std::isfinite(number), "Invalid numeric field");
    return number;
}

inline std::ofstream createFile(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << std::setprecision(17);
    return out;
}

class Recorder {
public:
    explicit Recorder(std::ostream& manifest) : log(manifest) {}
    ~Recorder() {
        cleanup();
    }

    void cleanup() noexcept {
        if (stopPending) {
            recordCleanup("GTN_WatchOff.cleanup", GTN_WatchOff(core));
            stopPending = false;
        }
        if (restoreGroup) {
            recordCleanup("GTN_SetWatchGroup.restore", GTN_SetWatchGroup(core, previousGroup));
            restoreGroup = false;
        }
        if (open) {
            recordCleanup("GTN_Close", GTN_Close());
            open = false;
        }
    }

    void check(const std::string& op, short rc) {
        log << op << ".rc=" << rc << '\n' << std::flush;
        if (rc != 0) throw std::runtime_error(op + " returned " + std::to_string(rc));
    }

    void connect() {
        const short rc = GTN_OpenCard(5, nullptr, nullptr);
        open = rc == 0;
        check("GTN_OpenCard", rc);
        TVersion firmware{};
        check("GTN_GetVersionEx", GTN_GetVersionEx(core, 1, &firmware));
        log << "firmware=" << firmware.version << '\n';
    }

    TWatchInfo info(const std::string& stage, short watchGroup = group) {
        TWatchInfo value{};
        check(stage + ".GTN_GetWatchInfoEx", GTN_GetWatchInfoEx(core, watchGroup, &value));
        log << stage << ".watch_group=" << watchGroup << "\n" << stage << ".enable=" << value.enable
            << "\n" << stage << ".run=" << value.run << "\n" << stage << ".time=" << value.time
            << "\n" << stage << ".head=" << value.head << "\n" << stage << ".threshold=" << value.threshold
            << "\n" << stage << ".interval=" << value.interval << "\n" << stage << ".mode=" << value.mode
            << "\n" << stage << ".count_before_event=" << value.countBeforeEvent
            << "\n" << stage << ".count_after_event=" << value.countAfterEvent
            << "\n" << stage << ".var_count=" << value.varCount
            << "\n" << stage << ".event_count=" << value.eventCount << '\n';
        return value;
    }

    void configure() {
        if (!restoreGroup) {
            check("GTN_GetWatchGroup", GTN_GetWatchGroup(core, &previousGroup));
            log << "previous_group=" << previousGroup << '\n';
        }
        const auto timer = info("before_timer");
        const auto background = info("before_background", WATCH_GROUP_BACKGROUND);
        require(!timer.enable && !timer.run && !background.enable && !background.run,
                "An existing Watch acquisition is active");
        restoreGroup = true;
        check("GTN_SetWatchGroup", GTN_SetWatchGroup(core, group));
        short selected = -1;
        check("selected.GTN_GetWatchGroup", GTN_GetWatchGroup(core, &selected));
        log << "selected_group=" << selected << '\n';
        require(selected == group, "Watch group readback mismatch");
        check("GTN_ClearWatch", GTN_ClearWatch(core, 0));
        short index = 0;
        for (const auto& channel : channels) {
            const auto name = "channel" + std::to_string(++index);
            check(name + ".GTN_AddWatchVar", GTN_AddWatchVar(core, &channel));
            TWatchVar actual{};
            TWatchVarInfo format{};
            check(name + ".GTN_GetWatchVar", GTN_GetWatchVar(core, index, &actual, &format));
            log << name << ".request=" << channel.type << ',' << channel.index << ',' << channel.id
                << "\n" << name << ".readback=" << actual.type << ',' << actual.index << ',' << actual.id
                << "\n" << name << ".layout=" << format.length << ',' << format.fraction << ','
                << format.format << ',' << format.hex << ',' << format.sign << ',' << format.bit << '\n';
            require(channel.type == actual.type && channel.index == actual.index &&
                    channel.id == actual.id, "Watch channel readback mismatch");
        }
        const auto configured = info("configured");
        require(configured.varCount == channels.size() && configured.eventCount == 0,
                "Unexpected Watch variable or event count");
    }

    void start() {
        stopPending = true;
        check("GTN_WatchOn", GTN_WatchOn(core, interval, WATCH_MODE_STATIC, watchCount));
    }

    void stop() {
        check("GTN_WatchOff", GTN_WatchOff(core));
        stopPending = false;
    }

    bool cleanupOk = true;
private:
    void recordCleanup(const char* name, short rc) noexcept {
        cleanupOk = cleanupOk && rc == 0;
        try {
            log << name << ".rc=" << rc << '\n' << std::flush;
        } catch (...) {
            cleanupOk = false;
        }
    }

    std::ostream& log;
    short previousGroup = 0;
    bool open = false, restoreGroup = false, stopPending = false;
};

struct CardClock {
    unsigned long ms = 0, tick = 0;
};

inline CardClock readClock(Recorder& recorder, std::ostream& log, const std::string& label) {
    CardClock clock;
    recorder.check(label + ".GTN_GetClock", GTN_GetClock(core, &clock.ms));
    recorder.check(label + ".GTN_GetClockHighPrecision", GTN_GetClockHighPrecision(core, &clock.tick));
    log << label << ".clock_ms=" << clock.ms << "\n" << label << ".clock_tick=" << clock.tick << '\n';
    return clock;
}

inline Ns verifyCorePeriod(Recorder& recorder, std::ostream& log) {
    double value = 0;
    short flag = 0, info = 0;
    recorder.check("GTN_GetCorePeriod", GTN_GetCorePeriod(core, &value, &flag, &info));
    log << "core_period_raw=" << value << "\ncore_period_flag=" << flag
        << "\ncore_period_info=" << info << '\n';
    const auto first = readClock(recorder, log, "calibration_start");
    std::this_thread::sleep_for(300ms);
    const auto last = readClock(recorder, log, "calibration_end");
    const auto ticks = last.tick - first.tick;
    const auto ms = last.ms - first.ms;
    require(ms >= 200 && ms <= 1000 && ticks > 0, "Could not calibrate the card clocks");
    const double measured = static_cast<double>(ms) / ticks;
    log << "core_period_from_card_clocks_ms=" << measured << '\n';
    const std::array<std::pair<const char*, double>, 3> units{{
        {"us", 1e-3}, {"ms", 1.0}, {"s", 1e3}
    }};
    for (const auto& unit : units) {
        const double period = value * unit.second;
        if (period > 0 && std::abs(period - measured) <= measured * 0.08) {
            log << "core_period_unit_verified=" << unit.first << '\n';
            const auto ns = static_cast<Ns>(std::llround(period * 1e6));
            require(ns > 0, "Core period is below supported resolution");
            log << "core_period_ns=" << ns << '\n' << std::flush;
            return ns;
        }
    }
    throw std::runtime_error("Core period unit unresolved by millisecond and interrupt clocks");
}

struct WatchData {
    std::array<std::vector<double>, channels.size()> columns;
    bool aligned = false;
};

inline WatchData readColumns(std::ostream& log, std::ostream& batches, Clock::time_point origin,
                      unsigned long capacity) {
    WatchData data;
    auto& columns = data.columns;
    std::array<unsigned long, channels.size()> counts{};
    bool allOk = true;
    for (std::size_t i = 0; i < columns.size(); ++i) {
        columns[i].resize(capacity, std::numeric_limits<double>::quiet_NaN());
        const auto begin = Clock::now();
        const short rc = GTN_ReadWatch(core, static_cast<short>(i + 1), columns[i].data(),
                                       capacity, &counts[i]);
        const auto end = Clock::now();
        const auto us = [origin](auto time) {
            return std::chrono::duration_cast<std::chrono::microseconds>(time - origin).count();
        };
        batches << i + 1 << ',' << channels[i].type << ',' << us(begin) << ',' << us(end)
                << ',' << counts[i] << ',' << rc << ",unavailable\n" << std::flush;
        log << "column" << i + 1 << ".GTN_ReadWatch.rc=" << rc << '\n' << std::flush;
        columns[i].resize(std::min(counts[i], capacity));
        allOk = allOk && rc == 0 && counts[i] < capacity;
    }
    data.aligned = allOk && counts[0] != 0 && counts[0] == counts[1] && counts[0] == counts[2];
    return data;
}

inline fs::path projectRoot() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(length != 0 && length < buffer.size(), "Cannot locate executable");
    buffer.resize(length);
    for (auto path = fs::path(buffer).parent_path(); path != path.root_path(); path = path.parent_path()) {
        if (fs::exists(path / L"ZRInertiaCurrent.vcxproj")) return path;
    }
    throw std::runtime_error("Run the executable in the project build tree");
}

inline std::string sessionId(const char* prefix = "Z-HS_") {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::ostringstream id;
    id << prefix << std::setfill('0') << std::setw(4) << t.wYear << std::setw(2)
       << t.wMonth << std::setw(2) << t.wDay << '-' << std::setw(2) << t.wHour
       << std::setw(2) << t.wMinute << std::setw(2) << t.wSecond << '_'
       << std::setw(3) << t.wMilliseconds << '_' << GetCurrentProcessId();
    return id.str();
}

}  // namespace watch_capture
