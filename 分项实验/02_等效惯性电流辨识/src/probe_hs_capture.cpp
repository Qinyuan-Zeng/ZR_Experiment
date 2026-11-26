#include "watch_capture.hpp"
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

namespace {
using namespace watch_capture;

Tick clockValue(double raw) {
    require(std::isfinite(raw) && raw == std::trunc(raw) &&
            raw >= INT32_MIN && raw <= UINT32_MAX, "Invalid card clock value");
    return static_cast<Tick>(static_cast<std::int64_t>(raw));
}

struct Frame {
    Tick tick;
    double position;
    int current;
};

Frame decode(double tick, double position, double current) {
    require(std::isfinite(position), "Invalid encoder value");
    require(std::isfinite(current) && current == std::trunc(current) &&
            current >= INT16_MIN && current <= INT16_MAX,
            "Current channel does not contain signed 16-bit raw values");
    return {clockValue(tick), position, static_cast<int>(current)};
}

// Count all full [start, start+width) windows on the integer ns time axis.
// A frame enters when start reaches t-width+1, and leaves at t+1.
std::pair<int, int> windowRange(const std::vector<Ns>& time, Ns width) {
    if (time.empty() || time.back() < width) return {-1, -1};
    const Ns lastStart = time.back() - width;
    std::size_t enter = 0, leave = 0;
    while (enter < time.size() && time[enter] < width) ++enter;
    int count = static_cast<int>(enter);
    int low = count, high = count;
    for (;;) {
        const Ns nextEnter = enter < time.size() ? time[enter] - width + 1 : INT64_MAX;
        const Ns nextLeave = leave < time.size() ? time[leave] + 1 : INT64_MAX;
        const Ns next = std::min(nextEnter, nextLeave);
        if (next > lastStart) break;
        while (enter < time.size() && time[enter] - width + 1 == next) {
            ++enter;
            ++count;
        }
        while (leave < time.size() && time[leave] + 1 == next) {
            ++leave;
            --count;
        }
        low = std::min(low, count);
        high = std::max(high, count);
    }
    return {low, high};
}

double quantile(const std::vector<Tick>& sorted, double p) {
    const double offset = p * static_cast<double>(sorted.size() - 1);
    const auto left = static_cast<std::size_t>(offset);
    const auto right = static_cast<std::size_t>(std::ceil(offset));
    return sorted[left] + (offset - static_cast<double>(left)) *
                          (sorted[right] - sorted[left]);
}

struct Timing {
    std::vector<Ns> time;
    std::vector<Tick> steps;
    std::map<std::size_t, std::size_t> equalRuns;
    std::size_t repeats = 0, backwards = 0, wraps = 0, gaps = 0;
    std::uint64_t missingTicks = 0;
};

Timing measure(const std::vector<Frame>& frames, Ns tickNs) {
    Timing timing;
    if (frames.empty()) return timing;
    timing.time.push_back(0);
    std::size_t run = 1;
    for (std::size_t i = 1; i < frames.size(); ++i) {
        const Tick step = frames[i].tick - frames[i - 1].tick;
        if (step >= 0x80000000u) {
            ++timing.backwards;
        } else {
            timing.steps.push_back(step);
            require(step < (INT64_MAX - timing.time.back()) / tickNs,
                    "Capture clock span exceeds supported range");
            timing.time.push_back(timing.time.back() + step * tickNs);
            if (step == 0) ++timing.repeats;
            if (frames[i].tick < frames[i - 1].tick) ++timing.wraps;
            if (step > 1) {
                ++timing.gaps;
                timing.missingTicks += step - 1;
            }
        }
        if (step == 1 && frames[i].current == frames[i - 1].current) {
            ++run;
        } else {
            ++timing.equalRuns[run];
            run = 1;
        }
    }
    ++timing.equalRuns[run];
    std::sort(timing.steps.begin(), timing.steps.end());
    return timing;
}

Timing summarize(std::ostream& out, const std::vector<Frame>& frames, Ns tickNs) {
    const auto timing = measure(frames, tickNs);
    const double tickMs = static_cast<double>(tickNs) / 1e6;
    out << std::setprecision(12) << "frames=" << frames.size() << "\ncore_period_ms=" << tickMs
        << "\nclock_duplicates=" << timing.repeats << "\nclock_backwards=" << timing.backwards
        << "\nclock_wraps=" << timing.wraps << "\ngap_intervals=" << timing.gaps
        << "\nmissing_core_ticks=" << timing.missingTicks << '\n';
    if (!timing.steps.empty()) {
        out << "frame_interval_median_ms=" << quantile(timing.steps, 0.5) * tickMs
            << "\nframe_interval_p95_ms=" << quantile(timing.steps, 0.95) * tickMs
            << "\nframe_interval_max_ms=" << timing.steps.back() * tickMs << '\n';
    }
    if (timing.backwards == 0 && !timing.time.empty()) {
        out << "card_span_ms=" << static_cast<double>(timing.time.back()) / 1e6 << '\n';
    }
    for (const Ns width : {5000000, 15000000, 23300000}) {
        out << "window_" << static_cast<double>(width) / 1e6 << "_ms_frames=";
        const auto range = timing.backwards == 0 ? windowRange(timing.time, width)
                                                 : std::make_pair(-1, -1);
        if (range.first < 0) out << "unavailable\n";
        else out << range.first << ".." << range.second << '\n';
    }
    out << "equal_current_run_frames:frequency";
    for (const auto& run : timing.equalRuns) out << ' ' << run.first << ':' << run.second;
    out << "\ncurrent_feedback_update_interval=unmeasured"
        << "\ncurrent_to_position_delay=unmeasured\n";
    return timing;
}

std::vector<Frame> replay(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    require(static_cast<bool>(std::getline(in, line)) && line == csvHeader,
            "Unrecognized frame CSV header");
    std::vector<Frame> frames;
    while (std::getline(in, line)) {
        std::istringstream row(line);
        std::array<double, 4> fields{};
        for (auto& field : fields) {
            std::string piece;
            require(static_cast<bool>(std::getline(row, piece, ',')), "Incomplete frame row");
            field = parseNumber(piece);
        }
        require(row.eof() && fields[0] == static_cast<double>(frames.size()),
                "Extra fields or out-of-order frame index");
        frames.push_back(decode(fields[1], fields[2], fields[3]));
    }
    require(in.eof() && !frames.empty(), "Frame CSV is unreadable or empty");
    return frames;
}

struct Reference {
    double position = 0, velocity = 0;
    short current = 0;
    long status = 0;
};

Reference reference(Recorder& recorder, std::ostream& log, const std::string& label) {
    Reference ref;
    recorder.check(label + ".GTN_GetAxisEncPos", GTN_GetAxisEncPos(core, axis, &ref.position));
    recorder.check(label + ".GTN_GetAxisEncVel", GTN_GetAxisEncVel(core, axis, &ref.velocity));
    recorder.check(label + ".GTN_GetAtlTorque", GTN_GetAtlTorque(core, axis, &ref.current));
    recorder.check(label + ".GTN_GetSts", GTN_GetSts(core, axis, &ref.status));
    log << label << ".position_pulse=" << ref.position << "\n" << label << ".velocity_pulse_ms="
        << ref.velocity << "\n" << label << ".current_raw=" << ref.current
        << "\n" << label << ".axis_status=" << ref.status << '\n';
    return ref;
}

void compare(std::ostream& out, const std::vector<Frame>& frames,
             const Reference& before, const Reference& after) {
    std::vector<double> pos;
    std::vector<int> current;
    for (const auto& frame : frames) {
        pos.push_back(frame.position);
        current.push_back(frame.current);
    }
    std::sort(pos.begin(), pos.end());
    std::sort(current.begin(), current.end());
    const auto medianPosition = pos[pos.size() / 2];
    const auto medianCurrent = current[current.size() / 2];
    out << "position_watch_min_median_max=" << pos.front() << ',' << medianPosition << ',' << pos.back()
        << "\nposition_ordinary_before_after=" << before.position << ',' << after.position
        << "\nposition_watch_minus_ordinary_before_after=" << medianPosition - before.position
        << ',' << medianPosition - after.position
        << "\ncurrent_watch_min_median_max=" << current.front() << ',' << medianCurrent << ',' << current.back()
        << "\ncurrent_ordinary_before_after=" << before.current << ',' << after.current
        << "\ncurrent_watch_minus_ordinary_before_after=" << medianCurrent - before.current
        << ',' << medianCurrent - after.current << '\n';
    out << "channel_mapping_review=compare_static_ranges_and_signed_values\n";
}

struct ExpectedMotion {
    double start = 0, reversal = 32, speed = 10, acceleration = 400;
};

bool readExpectedMotion(ExpectedMotion& expected) {
    std::cout << "Expected start, turnaround [mm], speed [mm/s], accel/decel [mm/s^2]"
                 " [0,32,10,400] (Q cancels): ";
    std::string input;
    if (!std::getline(std::cin, input) || input == "q" || input == "Q") return false;
    if (input.empty()) return true;
    std::istringstream row(input);
    for (double* entry : {&expected.start, &expected.reversal, &expected.speed, &expected.acceleration}) {
        std::string field;
        require(static_cast<bool>(std::getline(row, field, ',')), "Expected four comma-separated values");
        *entry = parseNumber(field);
    }
    require(row.eof(), "Expected four comma-separated values");
    require(expected.start >= 0 && expected.start <= 32 &&
            expected.reversal >= 0 && expected.reversal <= 32 &&
            expected.start != expected.reversal && expected.speed > 0 && expected.acceleration > 0,
            "Expected distinct positions within 0..32 mm and positive speed and acceleration");
    return true;
}

int capture(double seconds, const ExpectedMotion* expected) {
    const bool passiveMotion = expected != nullptr;
    const auto root = projectRoot();
    const auto id = sessionId();
    auto log = createFile(root / L"manifest" / (id + ".txt"));
    auto batches = createFile(root / L"data/raw" / (id + ".batches.csv"));
    auto raw = createFile(root / L"data/raw" / (id + ".csv"));
    auto result = createFile(root / L"results" / (id + ".summary.txt"));
    std::cout << "Session " << id << '\n';
    log << "session=" << id << "\nmode=" << (passiveMotion ? "passive_motion" : "static")
        << "\naxis_alias=a33\ncore=2\naxis=1\nconnection=PCIe_5\ncapture=Watch_timer_static"
        << "\nseconds_requested=" << seconds << "\ninterval_requested=" << interval
        << "\ncard_clock_code=1201\nencoder_code=30000\ncurrent_code=61201"
        << "\ncurrent_update_interval=unmeasured\nexplicit_overflow_status=unavailable"
        << "\nchannel_layout_fields=length,fraction,format,hex,sign,bit\n" << std::flush;
    if (expected) {
        log << "expected_source=operator_reference_actual_motion_from_encoder"
            << "\nexpected_start_mm=" << expected->start
            << "\nexpected_turnaround_mm=" << expected->reversal
            << "\nexpected_speed_mm_s=" << expected->speed
            << "\nexpected_accel_decel_mm_s2=" << expected->acceleration
            << "\nencoder_pulse_per_mm=1000\n" << std::flush;
    }
    batches << "column,channel_code,host_read_begin_us,host_read_end_us,frames,rc,overflow\n";
    raw << csvHeader << '\n' << std::flush;
    result << "session=" << id << "\nmode=" << (passiveMotion ? "passive_motion" : "static") << '\n';

    Recorder recorder(log);
    std::string outcome = "incomplete";
    int exitCode = 1;
    try {
        recorder.connect();
        const auto tickNs = verifyCorePeriod(recorder, log);
        const auto before = reference(recorder, log, "reference_before");
        if (!passiveMotion) {
            require(!(before.status & 0x400) && std::abs(before.velocity) <= 0.05,
                    "Axis is moving before a static capture");
        }
        // Static Watch uses the SDK's default count. Size the host read from the
        // requested span plus room for the host scheduling and stop latency.
        const double requestedCapacity = std::ceil((seconds + 0.5) * 1e9 /
                                                    static_cast<double>(tickNs));
        require(requestedCapacity <= maxReadCapacity, "Duration exceeds host Watch read capacity");
        const auto capacity = static_cast<unsigned long>(requestedCapacity);
        log << "watch_count_requested=" << watchCount << "\nread_capacity_per_channel="
            << capacity << '\n';
        recorder.configure();
        require(!stopRequested(), "Cancelled before starting Watch");
        const auto lowClock = readClock(recorder, log, "watch_clock_before");
        recorder.start();
        const auto start = Clock::now();
        std::this_thread::sleep_for(10ms);
        const auto running = recorder.info("started");
        require(running.mode == WATCH_MODE_STATIC && running.interval == interval && running.run &&
                running.varCount == channels.size(), "Watch did not start with the expected configuration");
        const auto deadline = start + std::chrono::duration<double>(seconds);
        if (passiveMotion) {
            std::cout << "采集已启动，可以开始本次往返。Q / Ctrl+C 结束采集。\n" << std::flush;
        } else {
            std::cout << "Capturing. Q or Ctrl+C stops Watch.\n" << std::flush;
        }
        int previousRemaining = -1;
        while (Clock::now() < deadline && !stopRequested()) {
            if (passiveMotion) {
                const auto remaining = std::chrono::duration<double>(deadline - Clock::now()).count();
                const int wholeSeconds = std::max(0, static_cast<int>(std::ceil(remaining)));
                if (wholeSeconds != previousRemaining) {
                    std::cout << "剩余采集时长 " << wholeSeconds << " s\n" << std::flush;
                    previousRemaining = wholeSeconds;
                }
            }
            std::this_thread::sleep_for(5ms);
        }
        const auto stopBegin = Clock::now();
        recorder.stop();
        const auto highClock = readClock(recorder, log, "watch_clock_after");
        const auto frozen = recorder.info("stopped");
        require(frozen.run == 0 && frozen.varCount == channels.size(), "Watch data is not frozen");
        log << "host_capture_elapsed_us="
            << std::chrono::duration_cast<std::chrono::microseconds>(stopBegin - start).count()
            << '\n' << std::flush;
        const auto after = reference(recorder, log, "reference_after");
        const auto data = readColumns(log, batches, start, capacity);

        // Preserve the exact SDK values before attempting to interpret them.
        const auto& columns = data.columns;
        const auto rows = std::max({columns[0].size(), columns[1].size(), columns[2].size()});
        for (std::size_t i = 0; i < rows; ++i) {
            raw << i;
            for (const auto& column : columns) {
                raw << ',';
                if (i < column.size()) raw << column[i];
            }
            raw << '\n';
        }
        raw.flush();
        const auto frozenAfterRead = recorder.info("after_read");
        const bool frozenDuringRead = frozenAfterRead.run == 0 &&
            frozenAfterRead.time == frozen.time && frozenAfterRead.head == frozen.head &&
            frozenAfterRead.varCount == frozen.varCount;
        require(data.aligned && frozenDuringRead,
                "Watch columns are empty, unequal, truncated, unstable or have a read error");
        const auto frames = replay(root / L"data/raw" / (id + ".csv"));
        const auto timing = summarize(result, frames, tickNs);
        compare(result, frames, before, after);
        const Tick clockSpan = highClock.tick - lowClock.tick;
        const bool clockMatch = clockSpan < 0x80000000u &&
            frames.front().tick - lowClock.tick <= clockSpan &&
            frames.back().tick - lowClock.tick <= clockSpan;
        result << "clock_matches_ordinary_bracket=" << clockMatch << '\n'
               << "overflow_flag=unavailable\n";
        const double spanMs = timing.time.empty() ? 0 :
            static_cast<double>(timing.time.back()) / 1e6;
        const bool fullSpan = spanMs >= seconds * 1000 - 2.0 * static_cast<double>(tickNs) / 1e6;
        result << "requested_duration_covered=" << fullSpan << '\n';
        const auto extrema = std::minmax_element(frames.begin(), frames.end(),
            [](const Frame& a, const Frame& b) { return a.position < b.position; });
        const bool stationary = !(after.status & 0x400) && std::abs(after.velocity) <= 0.05 &&
            extrema.second->position - extrema.first->position <= 10 &&
            std::abs(after.position - before.position) <= 10;
        if (!passiveMotion) result << "static_position_check=" << stationary << '\n';
        if (cancelled.load()) outcome = "operator_cancelled";
        else if (!clockMatch || timing.backwards || timing.repeats || timing.gaps ||
                 !fullSpan || (!passiveMotion && !stationary)) outcome = "review_required";
        else {
            outcome = "capture_complete_channel_review_pending";
            exitCode = 0;
        }
    } catch (const std::exception& e) {
        log << "error=" << e.what() << '\n';
        result << "error=" << e.what() << '\n';
        std::cerr << e.what() << '\n';
    }
    recorder.cleanup();
    if (!recorder.cleanupOk) {
        outcome = "cleanup_failed";
        exitCode = 1;
    }
    log << "outcome=" << outcome << '\n' << std::flush;
    result << "outcome=" << outcome << '\n' << std::flush;
    std::cout << "Outcome " << outcome << '\n';
    return exitCode;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 4 && std::wstring(argv[1]) == L"--replay") {
            const auto value = parseNumber(fs::path(argv[3]).string());
            require(value >= 1 && value <= 1000000000 && value == std::trunc(value),
                    "Replay requires a positive integer core period in ns");
            const auto timing = summarize(std::cout, replay(argv[2]), static_cast<Ns>(value));
            return timing.backwards == 0 ? 0 : 1;
        }
        const bool passiveMotion = argc == 2 && std::wstring(argv[1]) == L"--passive-motion";
        require(argc == 1 || passiveMotion,
                "Usage: ZRInertiaCurrent [--passive-motion | --replay <frames.csv> <core_period_ns>]");
        require(SetConsoleCtrlHandler(onCtrl, TRUE) != FALSE, "Cannot install console stop handler");
        if (passiveMotion) SetConsoleOutputCP(CP_UTF8);
        std::cout << "Read-only axis probe, a33 / core 2 / axis 1\n"
                  << (passiveMotion ? "Passive motion recording" : "Stationary recording")
                  << "\nDuration in seconds [" << (passiveMotion ? 10 : 5)
                  << ", range 1..10] (Q cancels): ";
        std::string input;
        if (!std::getline(std::cin, input) || input == "q" || input == "Q") return 0;
        const double seconds = input.empty() ? (passiveMotion ? 10.0 : 5.0) : parseNumber(input);
        require(seconds >= 1 && seconds <= 10, "Duration must be within 1..10 seconds");
        ExpectedMotion expected;
        if (passiveMotion) {
            if (!readExpectedMotion(expected)) return 0;
            std::cout << "Expected only: " << expected.start << " -> " << expected.reversal
                      << " -> " << expected.start << " mm, " << expected.speed << " mm/s, "
                      << expected.acceleration << " mm/s^2. Encoder data describes actual motion.\n";
        }
        std::cout << "Watch timer group on core 2 will be configured for " << seconds
                  << " s. Verify the axis and close other card clients. Type START: ";
        if (!std::getline(std::cin, input) || input != "START" || cancelled.load()) return 0;
        return capture(seconds, passiveMotion ? &expected : nullptr);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
