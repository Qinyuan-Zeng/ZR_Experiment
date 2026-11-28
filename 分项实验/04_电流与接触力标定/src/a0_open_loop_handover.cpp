#include "watch_capture.hpp"
#include <functional>

namespace {
using namespace watch_capture;
constexpr short controlIndex = 1, closedMode = 2, openMode = 3, adcChannel = 1;
constexpr long refPulse = 15670;
constexpr long enableMask = 1L << 9, movingMask = 1L << 10;
constexpr long faultMask = (1L << 1) | (1L << 5) | (1L << 6);
constexpr auto pollInterval = 5ms, switchTimeout = 300ms, holdDuration = 100ms, maxGap = 50ms;
constexpr char protocol[] = "Z-FA0-OPEN-15670-HANDOVER";

struct Settings {
    long holdDac;
    double displacementLimit, currentStepLimit, maxAbsAi1Voltage;
    long referencePulse = refPulse;
};

struct Sample {
    double x = 0, plan = 0, voltage = 0, target = 0;
    short current = 0, mode = 0, plus = 0, minus = 0;
    long status = 0;
    TSwitchLoopParameter task{};
    std::array<short, 9> rc{};
};

bool controlsReadable(const Sample& s) {
    return s.rc[2] == 0 && s.rc[3] == 0 && s.rc[6] == 0 && s.rc[7] == 0 &&
           (s.status & enableMask) && !(s.status & faultMask) && !s.plus && !s.minus;
}

bool axisAvailable(const Sample& s) {
    return controlsReadable(s) && s.rc[0] == 0 && s.rc[4] == 0 && s.rc[5] == 0 &&
           std::isfinite(s.x) && std::isfinite(s.voltage) &&
           (s.mode != closedMode || (s.rc[1] == 0 && std::isfinite(s.plan))) &&
           (s.mode != openMode || (s.rc[8] == 0 && std::isfinite(s.target)));
}

bool sameTask(const TSwitchLoopParameter& a, const TSwitchLoopParameter& b) {
    return a.enable == b.enable && a.mode == b.mode && a.dir == b.dir &&
           a.threshold == b.threshold && a.openLoopDac == b.openLoopDac && a.overTime == b.overTime &&
           a.posOffset == b.posOffset && a.reserve[0] == b.reserve[0] && a.reserve[1] == b.reserve[1];
}

Sample hardwareRead() {
    Sample s;
    s.rc[0] = GTN_GetAxisEncPos(core, axis, &s.x);
    s.rc[1] = GTN_GetPrfPos(core, axis, &s.plan);
    s.rc[2] = GTN_GetSts(core, axis, &s.status);
    s.rc[3] = GTN_GetControlLoopMode(core, axis, controlIndex, &s.mode);
    s.rc[4] = GTN_GetAtlTorque(core, axis, &s.current);
    s.rc[5] = GTN_GetAdc(core, adcChannel, &s.voltage);
    s.rc[6] = GTN_GetSwitchLoopParameter(core, axis, &s.task);
    s.rc[7] = GTN_GetLimitStatus(core, axis, &s.plus, &s.minus);
    if (s.rc[3] == 0 && s.mode == openMode)
        s.rc[8] = GTN_GetForceValue(core, axis, controlIndex, &s.target);
    return s;
}

struct Report {
    bool entered = false, held = false, returned = false, settingsRestored = false, settled = false;
    short entryRc = -1, cancelRc = -1, returnRc = -1, restoreRc = -1;
    const char* event = "not_started";
};

// The commissioning executable and the mock use the same state transitions.
template<class Read, class Write, class Now, class Pause, class Cancel>
Report handover(const Settings& cfg, Read read, Write write, Now now, Pause pause, Cancel cancel,
                const std::function<void()>& afterHold = {}) {
    Report report;
    const auto baseline = read("baseline");
    require(axisAvailable(baseline) && baseline.mode == closedMode && !baseline.task.enable &&
            !(baseline.status & movingMask) && std::abs(baseline.x - cfg.referencePulse) <= (cfg.referencePulse == 0 ? 1 : 10) &&
            std::abs(baseline.plan - baseline.x) <= 5 &&
            std::abs(baseline.voltage) < cfg.maxAbsAi1Voltage, "initial_state_rejected");

    // Verify the baseline over multiple samples without asking the loop to change.
    const auto baselineEnd = now() + 150ms;
    while (now() < baselineEnd) {
        require(!cancel(), "operator_cancelled_before_switch");
        pause();
        const auto s = read("baseline");
        require(axisAvailable(s) && s.mode == closedMode && !s.task.enable &&
                !(s.status & movingMask) && std::abs(s.x - baseline.x) <=
                    std::min(3.0, cfg.displacementLimit) &&
                std::abs(s.plan - baseline.plan) <= 1 &&
                std::abs(double(s.current) - baseline.current) <= cfg.currentStepLimit &&
                std::abs(s.voltage) < cfg.maxAbsAi1Voltage, "baseline_not_stable");
    }
    require(!cancel(), "operator_cancelled_before_switch");

    TSwitchLoopParameter entry{};
    entry.enable = 1;
    entry.mode = 0;
    entry.dir = 0;
    entry.threshold = 10;
    entry.openLoopDac = cfg.holdDac;
    entry.overTime = 10;
    auto last = now(), deadline = last + switchTimeout;
    report.entryRc = write(entry);
    report.event = report.entryRc ? "entry_write_failed_or_uncertain" : "complete";
    while (report.entryRc == 0) {
        if (cancel()) { report.event = "operator_stop"; break; }
        const auto s = read("handover");
        const auto stamp = now();
        if (!axisAvailable(s)) { report.event = "status_limit_or_read_failure"; break; }
        if (stamp - last > maxGap) { report.event = "sample_gap"; break; }
        if (std::abs(s.x - baseline.x) > cfg.displacementLimit) { report.event = "displacement_limit"; break; }
        if (std::abs(s.voltage) >= cfg.maxAbsAi1Voltage) { report.event = "ai1_voltage_limit"; break; }
        if (std::abs(double(s.current) - baseline.current) > cfg.currentStepLimit) {
            report.event = "feedback_current_step_limit";
            break;
        }
        if (!report.entered && stamp >= deadline) { report.event = "entry_timeout"; break; }
        if (s.mode == openMode && !s.task.enable && !report.entered) {
            if (s.target != cfg.holdDac) { report.event = "target_readback_mismatch"; break; }
            report.entered = true;
            deadline = stamp + holdDuration;
        }
        if (report.entered && (s.mode != openMode || s.task.enable || s.target != cfg.holdDac)) {
            report.event = "mode_or_target_changed";
            break;
        }
        if (!report.entered && s.mode != closedMode && s.mode != openMode) {
            report.event = "unexpected_mode";
            break;
        }
        if (stamp >= deadline) {
            report.held = report.entered;
            if (!report.held) report.event = "entry_timeout";
            break;
        }
        last = stamp;
        pause();
    }

    if (report.held && afterHold) {
        try {
            afterHold();
        } catch (...) {
            report.held = false;
            report.event = "open_loop_measurement_failed";
        }
    }

    // Cancel a pending entry if still in position mode. Re-read because the task
    // could have completed between the last observation and cancellation.
    auto s = read("return_check");
    if (controlsReadable(s) && s.mode == closedMode && s.task.enable) {
        report.cancelRc = write(baseline.task);
        s = read("after_cancel");
    }
    if (controlsReadable(s) && s.mode == openMode) {
        TSwitchLoopParameter back{};
        back.enable = 1;
        back.mode = 1;
        back.posOffset = 0;
        const auto until = now() + switchTimeout;
        report.returnRc = write(back);
        do {
            s = read("returning");
            if (!controlsReadable(s) || (s.mode == closedMode && !s.task.enable)) break;
            pause();
        } while (now() < until);
    }
    report.returned = controlsReadable(s) && s.mode == closedMode && !s.task.enable;
    if (report.returned) {
        report.restoreRc = write(baseline.task);
        s = read("restored");
        report.settingsRestored = report.restoreRc == 0 && axisAvailable(s) &&
                                  s.mode == closedMode && sameTask(baseline.task, s.task);
        const double standstillPosition = s.x;
        const auto settledUntil = now() + 150ms;
        report.settled = report.settingsRestored;
        while (report.settled && now() < settledUntil) {
            pause();
            s = read("after_return");
            report.settled = axisAvailable(s) && s.mode == closedMode && !s.task.enable &&
                !(s.status & movingMask) && std::abs(s.x - standstillPosition) <= 3 &&
                std::abs(s.x - s.plan) <= 5;
        }
    }
    return report;
}

void selfTest() {
    const Settings cfg{123, 5, 5, .010};
    for (const int scenario : {0, 1, 2, 3}) {
        auto time = Clock::time_point{};
        Sample s{};
        s.x = s.plan = refPulse;
        s.current = 488;
        s.mode = closedMode;
        s.status = enableMask;
        bool sawReturn = false, sawCancel = false;
        auto read = [&](const char*) { return s; };
        auto write = [&](TSwitchLoopParameter task) -> short {
            if (task.enable && task.mode == 0) {
                if (scenario == 3) {
                    s.rc[3] = 1;
                } else if (scenario == 2) {
                    s.task = task;  // Pending task requires cancellation.
                } else {
                    s.mode = openMode;
                    s.target = task.openLoopDac;
                    if (scenario == 1) s.current += 10;
                }
            } else if (task.enable && task.mode == 1) {
                sawReturn = true;
                require(task.posOffset == 0, "Return must not request additional displacement");
                s.mode = closedMode;
                s.target = 0;
                s.task = {};
            } else {
                sawCancel = sawCancel || s.task.enable != 0;
                s.task = task;
            }
            return 0;
        };
        const auto out = handover(cfg, read, write, [&] { return time; },
                                  [&] { time += pollInterval; }, [] { return false; });
        if (scenario == 0)
            require(out.held && out.returned && out.settingsRestored && out.settled && sawReturn, "Normal mock failed");
        if (scenario == 1)
            require(!out.held && out.returned && sawReturn &&
                    std::string(out.event) == "feedback_current_step_limit", "Current guard mock failed");
        if (scenario == 2)
            require(!out.held && out.returned && sawCancel && !sawReturn, "Pending-cancel mock failed");
        if (scenario == 3)
            require(!out.held && !out.returned && !sawReturn, "Link-loss mock falsely claimed return");
    }
    std::cout << "Mock handover: normal, overcurrent step, pending cancellation and link loss verified.\n";
}

int commissioning(const Settings& cfg) {
    const auto root = projectRoot(L"ZRForceAcquisition.vcxproj");
    const auto id = sessionId("Z-FA0-OLHAND_");
    auto log = createFile(root / L"manifest" / (id + ".txt"));
    auto csv = createFile(root / L"data/raw" / (id + ".handover.csv"));
    log << "session=" << id << "\nprotocol=" << protocol
        << "\noperator_confirmation=HANDOVER\nhold_dac=" << cfg.holdDac
        << "\ndisplacement_limit_pulse=" << cfg.displacementLimit
        << "\ncurrent_step_limit_raw=" << cfg.currentStepLimit
        << "\nmax_abs_ai1_voltage_v=" << cfg.maxAbsAi1Voltage
        << "\nbuild=" << __DATE__ << ' ' << __TIME__ << '\n' << std::flush;
    struct Row { const char* phase; long long hostUs; Sample value; };
    std::vector<Row> rows;
    rows.reserve(2048);
    Recorder card(log);
    Report result;
    try {
        card.connect();
        const auto begin = Clock::now();
        auto read = [&](const char* phase) {
            const auto s = hardwareRead();
            const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count();
            if (rows.size() < rows.capacity()) rows.push_back({phase, stamp, s});
            return s;
        };
        result = handover(cfg, read,
            [](TSwitchLoopParameter prm) { return GTN_SetSwitchLoopParameter(core, axis, &prm); },
            [] { return Clock::now(); }, [] { std::this_thread::sleep_for(pollInterval); }, stopRequested);
    } catch (const std::exception& error) {
        log << "error=" << error.what() << '\n';
        std::cerr << error.what() << '\n';
    }
    card.cleanup();
    csv << "phase,host_us,position_pulse,planned_pulse,current_raw,ai1_v,loop_mode,switch_pending,dac_readback,"
           "axis_status,positive_limit,negative_limit,pos_rc,plan_rc,status_rc,mode_rc,current_rc,adc_rc,switch_rc,limit_rc,target_rc\n";
    for (const auto& row : rows) {
        const auto& s = row.value;
        csv << row.phase << ',' << row.hostUs << ',' << s.x << ',' << s.plan << ',' << s.current
            << ',' << s.voltage << ',' << s.mode << ',' << s.task.enable << ',';
        if (s.mode == openMode && s.rc[8] == 0) csv << s.target;
        csv << ',' << s.status << ',' << s.plus << ',' << s.minus;
        for (const auto rc : s.rc) csv << ',' << rc;
        csv << '\n';
    }
    csv.flush();
    const bool ok = result.held && result.returned && result.settingsRestored && result.settled && card.cleanupOk;
    log << "entry_rc=" << result.entryRc << "\ncancel_rc=" << result.cancelRc << "\nreturn_rc=" << result.returnRc
        << "\nrestore_rc=" << result.restoreRc << "\nentered=" << result.entered << "\nheld=" << result.held
        << "\nclosed_mode_confirmed=" << result.returned << "\nsettings_restored=" << result.settingsRestored
        << "\nstationary_after_return=" << result.settled
        << "\nreason=" << result.event << "\noutcome="
        << (ok ? "handover_observed_pending_independent_review" : "incomplete") << '\n' << std::flush;
    if (result.entryRc != -1 && !result.returned)
        std::cerr << "Closed-loop return unconfirmed. Use the separately verified site recovery.\n";
    std::cout << id << (ok ? " handover_observed_pending_independent_review" : " incomplete") << '\n';
    return ok ? 0 : 1;
}
struct DacMapSettings {
    long first, increment, levels, holdMs;
    long lowerDac, upperDac, maxAbsCurrent;
    double maxDisplacement, maxAbsVoltage;
    long referencePulse = refPulse;
};

struct DacSample {
    std::size_t step;
    long command;
    long long elapsedUs;
    Sample data;
};

struct DacPoint {
    std::size_t step;
    long dac;
    double currentMedian, forceVoltageMedian;
    short currentMin, currentMax;
    std::size_t samples;
};

constexpr char dacMapProtocol[] = "Z-FA0-OPEN-DAC-CURRENT-01";
constexpr char zeroEndProtocol[] = "Z-FA0-OPEN-ZERO-DAC-01";

std::vector<long> dacMapPlan(const DacMapSettings& cfg) {
    require(cfg.first >= -32768 && cfg.first <= 32767 && cfg.increment != 0 &&
            cfg.increment >= -65535 && cfg.increment <= 65535 &&
            cfg.levels >= 2 && cfg.levels <= 12 && cfg.holdMs >= 1000 && cfg.holdMs <= 3000,
            "Use 2-12 DAC levels and 1000-3000 ms per hold");
    require(cfg.lowerDac >= -32768 && cfg.upperDac <= 32767 && cfg.lowerDac <= cfg.upperDac &&
            cfg.maxAbsCurrent > 0 && cfg.maxAbsCurrent <= 32767 &&
            std::isfinite(cfg.maxDisplacement) && cfg.maxDisplacement > 0 &&
            std::isfinite(cfg.maxAbsVoltage) && cfg.maxAbsVoltage > 0 && cfg.maxAbsVoltage < 9.99,
            "Invalid site-approved DAC or feedback limits");
    std::vector<long> plan;
    for (long i = 0; i < cfg.levels; ++i) {
        const long dac = cfg.first + i * cfg.increment;
        require(dac >= cfg.lowerDac && dac <= cfg.upperDac, "DAC point outside site-approved limits");
        plan.push_back(dac);
    }
    for (long i = cfg.levels - 2; i >= 0; --i) plan.push_back(plan[static_cast<std::size_t>(i)]);
    return plan;
}

void validateDacMapSample(const DacMapSettings& cfg, const Sample& s, double start, long dac) {
    require(axisAvailable(s) && s.mode == openMode && !s.task.enable && !(s.status & movingMask),
            "open_loop_status_or_telemetry");
    require(s.target == dac, "dac_readback_mismatch");
    require(std::abs(s.x - start) <= cfg.maxDisplacement, "dac_map_displacement_limit");
    require(std::abs(static_cast<int>(s.current)) < cfg.maxAbsCurrent, "dac_map_current_limit");
    require(std::abs(s.voltage) < cfg.maxAbsVoltage, "dac_map_ai1_voltage_limit");
}

double medianSorted(const std::vector<double>& values) {
    require(!values.empty(), "No samples in the steady window");
    const auto n = values.size();
    return (values[(n - 1) / 2] + values[n / 2]) / 2;
}

template<class Read, class Write, class Now, class Pause, class Cancel>
void measureDacMap(const DacMapSettings& cfg, const std::vector<long>& plan,
                   std::vector<DacPoint>& points, Read read, Write write,
                   Now now, Pause pause, Cancel cancel) {
    require(!cancel(), "operator_stop");
    const auto initial = read(0, plan.front());
    const auto tolerance = cfg.referencePulse == 0 ? 1 : 10;
    require(std::isfinite(initial.x) && std::abs(initial.x - cfg.referencePulse) <= tolerance,
            "expected_dac_map_start_position");
    validateDacMapSample(cfg, initial, initial.x, plan.front());
    auto lastRead = now();
    std::vector<double> currents, voltages;
    currents.reserve(256);
    voltages.reserve(256);
    for (std::size_t step = 0; step < plan.size(); ++step) {
        const long dac = plan[step];
        if (step) {
            require(!cancel(), "operator_stop");
            const auto before = read(step, plan[step - 1]);
            validateDacMapSample(cfg, before, initial.x, plan[step - 1]);
            require(now() - lastRead <= maxGap, "dac_map_sample_gap");
            lastRead = now();
            require(!cancel(), "operator_stop");
            require(write(step, dac) == 0, "dac_write_failed_or_uncertain");
        }
        currents.clear();
        voltages.clear();
        const auto deadline = now() + std::chrono::milliseconds(cfg.holdMs);
        do {
            require(!cancel(), "operator_stop");
            const auto s = read(step, dac);
            const auto stamp = now();
            validateDacMapSample(cfg, s, initial.x, dac);
            require(stamp - lastRead <= maxGap, "dac_map_sample_gap");
            lastRead = stamp;
            if (deadline - stamp <= 500ms) {
                currents.push_back(s.current);
                voltages.push_back(s.voltage);
            }
            if (stamp >= deadline) break;
            pause();
        } while (true);
        require(currents.size() >= 10, "insufficient_dac_plateau_tail_samples");
        std::sort(currents.begin(), currents.end());
        std::sort(voltages.begin(), voltages.end());
        points.push_back({step, dac, medianSorted(currents), medianSorted(voltages),
                          static_cast<short>(currents.front()), static_cast<short>(currents.back()),
                          currents.size()});
    }
}

void dacMapSelfTest() {
    const DacMapSettings cfg{100, 2, 3, 1000, 100, 104, 100, 5, .02};
    const auto plan = dacMapPlan(cfg);
    require(plan == std::vector<long>{100, 102, 104, 102, 100}, "DAC return path wrong");
    auto outOfRange = cfg;
    outOfRange.upperDac = 103;
    bool rejected = false;
    try { dacMapPlan(outOfRange); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "Unsafe DAC point was accepted");

    for (const bool fault : {false, true}) {
        auto now = Clock::time_point{};
        Sample s{};
        s.x = refPulse;
        s.mode = openMode;
        s.status = enableMask;
        s.target = plan.front();
        s.current = 15;
        std::vector<long> writes;
        std::vector<DacPoint> points;
        bool interrupted = false;
        try {
            measureDacMap(cfg, plan, points,
                [&](std::size_t, long) { return s; },
                [&](std::size_t, long dac) -> short {
                    writes.push_back(dac);
                    s.target = dac;
                    s.current = fault ? 100 : static_cast<short>(dac - 85);
                    return 0;
                },
                [&] { return now; }, [&] { now += 20ms; }, [] { return false; });
        } catch (const std::runtime_error&) {
            interrupted = true;
        }
        if (!fault) {
            require(!interrupted && writes == std::vector<long>{102, 104, 102, 100} &&
                    points.size() == plan.size() && points[2].currentMedian == 19,
                    "DAC staircase, tail summary or initial-state restoration failed");
        } else {
            require(interrupted && writes == std::vector<long>{102},
                    "A limit trip sent subsequent DAC commands");
        }
    }
    std::cout << "Mock DAC map: ascending/return sequence, steady median and limit interruption verified.\n";
}

int commissionDacMap(const DacMapSettings& cfg, const std::vector<long>& plan, bool zeroEnd = false) {
    const auto root = projectRoot(L"ZRForceAcquisition.vcxproj");
    const auto id = sessionId("Z-FA0-DACMAP_");
    auto log = createFile(root / L"manifest" / (id + ".txt"));
    auto csv = createFile(root / L"data/raw" / (id + ".dac_current.csv"));
    auto summary = createFile(root / L"results" / (id + ".dac_current_points.csv"));
    log << "session=" << id << "\nprotocol=" << (zeroEnd ? zeroEndProtocol : dacMapProtocol)
        << "\noperator_confirmation=" << (zeroEnd ? "ZEROEND" : "DACMAP")
        << "\nreference_pulse=" << cfg.referencePulse
        << "\nstart_dac=" << cfg.first << "\nincrement_dac=" << cfg.increment << "\nlevels=" << cfg.levels
        << "\nhold_ms=" << cfg.holdMs << "\nminimum_dac=" << cfg.lowerDac << "\nmaximum_dac=" << cfg.upperDac
        << "\nmax_abs_current_raw=" << cfg.maxAbsCurrent << "\nmax_displacement_pulse=" << cfg.maxDisplacement
        << "\nmax_abs_ai1_v=" << cfg.maxAbsVoltage << "\nplan_dac=";
    for (const long dac : plan) log << dac << ' ';
    log << '\n' << std::flush;
    Recorder card(log);
    std::vector<DacSample> samples;
    samples.reserve(20000);
    std::vector<DacPoint> points;
    points.reserve(plan.size());
    struct Command { std::size_t step; long target; short rc; };
    std::vector<Command> commands;
    commands.reserve(plan.size());
    std::vector<std::pair<short, short>> switches;
    switches.reserve(4);
    bool complete = false;
    Report zeroEndReport;
    std::string sweepErrorText;
    sweepErrorText.reserve(256);
    try {
        card.connect();
        const auto origin = Clock::now();
        auto read = [&](std::size_t step, long dac) {
                const auto s = hardwareRead();
                const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - origin).count();
                require(samples.size() < samples.capacity(), "DAC map trace buffer exhausted");
                samples.push_back({step, dac, stamp, s});
            return s;
        };
        auto writeDac = [&](std::size_t step, long dac) {
            const short rc = GTN_SetForceValue(core, axis, controlIndex, dac);
            commands.push_back({step, dac, rc});
            return rc;
        };
        const auto now = [] { return Clock::now(); };
        const auto pause = [] { std::this_thread::sleep_for(pollInterval); };
        const auto sweep = [&] { measureDacMap(cfg, plan, points, read, writeDac, now, pause, stopRequested); };
        if (zeroEnd) {
            const Settings handoverLimits{0, cfg.maxDisplacement, static_cast<double>(cfg.maxAbsCurrent),
                                          cfg.maxAbsVoltage, cfg.referencePulse};
            const auto beforeSwitch = read(plan.size(), 0);
            require(axisAvailable(beforeSwitch) && beforeSwitch.mode == closedMode &&
                    std::abs(static_cast<int>(beforeSwitch.current)) < cfg.maxAbsCurrent,
                    "zero_end_baseline_outside_current_limit");
            zeroEndReport = handover(handoverLimits, [&](const char*) { return read(plan.size(), 0); },
                [&](TSwitchLoopParameter task) {
                    const short rc = GTN_SetSwitchLoopParameter(core, axis, &task);
                    switches.emplace_back(task.mode, rc);
                    return rc;
                }, now, pause, stopRequested, [&] {
                    try {
                        sweep();
                    } catch (const std::exception& error) {
                        sweepErrorText = error.what();
                        throw;
                    }
                });
            complete = zeroEndReport.held && zeroEndReport.returned && zeroEndReport.settingsRestored &&
                       zeroEndReport.settled && points.size() == plan.size();
        } else {
            sweep();
            complete = true;
        }
    } catch (const std::exception& error) {
        log << "error=" << error.what() << '\n' << std::flush;
        std::cerr << error.what() << '\n';
    }
    card.cleanup();

    if (zeroEnd) {
        log << "handover_event=" << zeroEndReport.event << "\nclosed_mode_confirmed=" << zeroEndReport.returned
            << "\nstationary_after_return=" << zeroEndReport.settled << '\n';
        if (!sweepErrorText.empty()) log << "sweep_error=" << sweepErrorText << '\n';
        for (std::size_t i = 0; i < switches.size(); ++i)
            log << "switch." << i << ".task_mode=" << switches[i].first
                << "\nswitch." << i << ".rc=" << switches[i].second << '\n';
    }
    for (const auto& cmd : commands)
        log << "set." << cmd.step << ".dac=" << cmd.target << "\nset." << cmd.step << ".rc=" << cmd.rc << '\n';
    csv << "step,command_dac,host_us,position_pulse,mode,target_readback_dac,current_feedback_raw,ai1_v,"
           "axis_status,switch_pending,positive_limit,negative_limit,pos_rc,plan_rc,status_rc,mode_rc,"
           "current_rc,adc_rc,switch_rc,limit_rc,target_rc\n";
    for (const auto& row : samples) {
        const auto& s = row.data;
        csv << row.step << ',' << row.command << ',' << row.elapsedUs << ',' << s.x << ',' << s.mode << ',';
        if (s.mode == openMode && s.rc[8] == 0) csv << s.target;
        csv << ',' << s.current << ',' << s.voltage << ',' << s.status << ',' << s.task.enable
            << ',' << s.plus << ',' << s.minus;
        for (const auto rc : s.rc) csv << ',' << rc;
        csv << '\n';
    }
    summary << "step,dac,current_median_raw,current_min_raw,current_max_raw,ai1_median_v,tail_samples,nominal_dac_v_10v_scale\n";
    for (const auto& point : points)
        summary << point.step << ',' << point.dac << ',' << point.currentMedian << ',' << point.currentMin << ','
                << point.currentMax << ',' << point.forceVoltageMedian << ',' << point.samples
                << ',' << point.dac * (10.0 / 32768) << '\n';
    csv.flush();
    summary.flush();
    complete = complete && card.cleanupOk;
    log << "outcome=" << (complete ? "complete_start_dac_confirmed" : "incomplete") << '\n' << std::flush;
    if (!complete && (!commands.empty() || !switches.empty()))
        std::cerr << "Initial DAC restoration not confirmed. Use the verified independent site recovery.\n";
    std::cout << id << (complete ? " complete_start_dac_confirmed" : " incomplete") << '\n';
    return complete ? 0 : 1;
}

void zeroEndSelfTest() {
    const DacMapSettings cfg{0, 20, 6, 1000, 0, 100, 50, 4, .02, 0};
    const auto plan = dacMapPlan(cfg);
    require(plan == std::vector<long>{0,20,40,60,80,100,80,60,40,20,0}, "Zero-end DAC plan mismatch");
    for (const bool trip : {false, true}) {
        Sample sample{};
        sample.mode = closedMode;
        sample.status = enableMask;
        auto time = Clock::time_point{};
        std::vector<DacPoint> points;
        std::vector<long> sent;
        auto now = [&] { return time; };
        auto pause = [&] { time += 20ms; };
        auto mockCancel = [] { return false; };
        auto switchLoop = [&](TSwitchLoopParameter task) -> short {
            if (task.enable && task.mode == 0) {
                require(task.openLoopDac == 0, "Zero-end initial DAC was not zero");
                sample.mode = openMode;
                sample.target = task.openLoopDac;
            } else if (task.enable && task.mode == 1) {
                sample.mode = closedMode;
                sample.current = 0;
            }
            return 0;
        };
        const auto report = handover({0, 4, 50, .02, 0},
            [&](const char*) { return sample; }, switchLoop, now, pause, mockCancel,
            [&] {
                measureDacMap(cfg, plan, points, [&](std::size_t, long) { return sample; },
                    [&](std::size_t, long dac) -> short {
                        sent.push_back(dac);
                        sample.target = dac;
                        sample.current = trip && dac == 40 ? 50 : static_cast<short>(dac / 8);
                        return 0;
                    }, now, pause, mockCancel);
            });
        require(report.returned && report.settingsRestored && report.settled, "Zero-end return not verified");
        if (trip) {
            require(!report.held && sent == std::vector<long>{20, 40},
                    "Zero-end limit failure allowed a further DAC step");
        } else {
            require(report.held && points.size() == plan.size() && sent.back() == 0,
                    "Zero-end sweep did not finish at a confirmed zero");
        }
    }
    std::cout << "Zero-end mock: mode 2->3->2, full 0..100..0 and current-trip recovery verified.\n";
}

int zeroDacMapPrompt() {
    std::cout << zeroEndProtocol << "\n0 pulse, initially closed loop, positive DAC sweep from 0 to 100.\n"
                 "Enter approved max absolute current raw, max displacement pulse and max absolute AI1 V: ";
    std::string line, extra;
    if (!std::getline(std::cin, line) || line == "Q" || line == "q") return 0;
    DacMapSettings cfg{0, 20, 6, 1000, 0, 100, 0, 0, 0, 0};
    std::istringstream values(line);
    require(bool(values >> cfg.maxAbsCurrent >> cfg.maxDisplacement >> cfg.maxAbsVoltage) &&
            !(values >> extra), "Expected three site-approved protection values");
    const auto plan = dacMapPlan(cfg);
    std::cout << "DAC levels:";
    for (const long dac : plan) std::cout << ' ' << dac;
    std::cout << "\nConfirm retract stop and independent site recovery. Enter ZEROEND or Q: ";
    if (!std::getline(std::cin, line) || line != "ZEROEND") return 0;
    require(SetConsoleCtrlHandler(onCtrl, TRUE), "Cannot install console cancellation handler");
    return commissionDacMap(cfg, plan, true);
}

int dacMapPrompt() {
    std::cout << dacMapProtocol << "\nAlready-open mode 3 with independently verified mechanical restraint "
                 "and site recovery is required.\n"
                 "Input verified initial DAC, signed step, number of levels and hold time in ms: ";
    std::string line, extra;
    if (!std::getline(std::cin, line) || line == "Q" || line == "q") return 0;
    std::istringstream recipe(line);
    DacMapSettings cfg{};
    require(bool(recipe >> cfg.first >> cfg.increment >> cfg.levels >> cfg.holdMs) &&
            !(recipe >> extra), "Expected four integer parameters");
    std::cout << "Input site-approved min DAC, max DAC, max absolute current raw, displacement pulse "
                 "and absolute AI1 V: ";
    if (!std::getline(std::cin, line) || line == "Q" || line == "q") return 0;
    std::istringstream limits(line);
    require(bool(limits >> cfg.lowerDac >> cfg.upperDac >> cfg.maxAbsCurrent >>
                 cfg.maxDisplacement >> cfg.maxAbsVoltage) && !(limits >> extra),
            "Expected five limits");
    const auto plan = dacMapPlan(cfg);
    std::cout << "DAC levels:";
    for (const long dac : plan) std::cout << ' ' << dac;
    std::cout << "\nConfirm verified site restraint and recovery at 15670 pulse. Enter DACMAP or Q: ";
    if (!std::getline(std::cin, line) || line != "DACMAP") return 0;
    require(SetConsoleCtrlHandler(onCtrl, TRUE), "Cannot install console cancellation handler");
    return commissionDacMap(cfg, plan);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const std::string option = argc == 2 ? argv[1] : "";
        if (option == "--self-test") {
            selfTest();
            dacMapSelfTest();
            zeroEndSelfTest();
            return 0;
        }
        if (option == "--dac-map") return dacMapPrompt();
        if (option == "--zero-dac-map") return zeroDacMapPrompt();
        if (argc != 1 && option != "--commission") {
            std::cout << "Usage: ZRForceOpenLoopHandover.exe --self-test | --commission | --dac-map | --zero-dac-map\n";
            return 2;
        }
        std::cout << protocol << "\nInput signed hold DAC verified on this axis: ";
        std::string line;
        if (!std::getline(std::cin, line) || line == "Q") return 0;
        const auto dac = parseNumber(line);
        require(dac != 0 && dac == std::trunc(dac) && dac >= -32768 && dac <= 32767,
                "Expected a verified nonzero signed whole-number DAC");
        std::cout << "Input site-approved max displacement (pulse), current step (raw), "
                     "and absolute AI1 voltage (V), separated by spaces: ";
        if (!std::getline(std::cin, line) || line == "Q") return 0;
        std::istringstream fields(line);
        Settings cfg{static_cast<long>(dac), 0, 0, 0};
        std::string extra;
        require(bool(fields >> cfg.displacementLimit >> cfg.currentStepLimit >> cfg.maxAbsAi1Voltage) &&
                !(fields >> extra) && std::isfinite(cfg.displacementLimit) &&
                std::isfinite(cfg.currentStepLimit) && std::isfinite(cfg.maxAbsAi1Voltage) &&
                cfg.displacementLimit > 0 && cfg.currentStepLimit > 0 &&
                cfg.maxAbsAi1Voltage > 0 && cfg.maxAbsAi1Voltage < 9.99,
                "Expected three positive limits within the valid AI1 voltage range");
        std::cout << "At 15670 pulse unloaded, confirm independent site recovery, limits and "
                     "the verified hold DAC. Enter HANDOVER to commission, Q to quit: ";
        if (!std::getline(std::cin, line) || line != "HANDOVER") return 0;
        require(SetConsoleCtrlHandler(onCtrl, TRUE), "Cannot install cancellation handler");
        return commissioning(cfg);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
