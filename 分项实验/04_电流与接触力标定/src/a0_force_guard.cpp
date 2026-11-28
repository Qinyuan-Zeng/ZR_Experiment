#include "matched_motion.hpp"

namespace {
using namespace matched_motion;
constexpr char protocol[] = "Z-FA0-SPRING-01-PILOT";
constexpr short adcChannel = 1;
constexpr double adcUsableMagnitudeV = 9.99;
constexpr double mappingThresholdG = 200.0;
constexpr long startPulse = 15670, targetPulse = 16670;
constexpr double speed = 0.5, acceleration = 20, followLimit = 500;
constexpr long long maxGapUs = 50000;
constexpr auto pollWait = 5ms;

enum class Trip { none, force, adcInvalid, hostGap, follow, target, operatorStop, timeout };

const char* name(Trip trip) {
    switch (trip) {
    case Trip::none: return "none";
    case Trip::force: return "force_threshold";
    case Trip::adcInvalid: return "adc_invalid_or_stale";
    case Trip::hostGap: return "monitor_gap";
    case Trip::follow: return "following_error";
    case Trip::target: return "target_reached_without_force_trip";
    case Trip::operatorStop: return "operator_stop";
    case Trip::timeout: return "motion_timeout";
    }
    return "unknown";
}

struct Calibration {
    double zeroV, gramsPerV, tripGrams;
    double grams(double volts) const { return (volts - zeroV) * gramsPerV; }

    bool thresholdWithinAdcRange(double thresholdG) const {
        const double offsetV = thresholdG / std::abs(gramsPerV);
        return std::abs(zeroV) + offsetV < adcUsableMagnitudeV;
    }
};

Calibration makeCalibration(double zeroV, double referenceV, double referenceGrams, double tripGrams) {
    const double deltaV = referenceV - zeroV;
    require(std::abs(zeroV) < adcUsableMagnitudeV && std::abs(referenceV) < adcUsableMagnitudeV &&
            std::abs(deltaV) >= .01 && referenceGrams >= 15 && referenceGrams <= 200 &&
            referenceGrams > tripGrams, "Invalid reference pair for the shallow press");
    const Calibration calibration{zeroV, referenceGrams / deltaV, tripGrams};
    require(calibration.thresholdWithinAdcRange(tripGrams), "Pilot threshold outside ADC range");
    return calibration;
}

struct Sample {
    long long hostUs = 0;
    unsigned long adcClock = 0;
    short adcRc = 0;
    double volts = 0, forceG = 0, position = 0, planned = 0, following = 0;
};

bool validAdc(const Sample& sample) {
    return sample.adcRc == 0 && std::isfinite(sample.volts) && std::abs(sample.volts) < adcUsableMagnitudeV &&
           std::isfinite(sample.forceG);
}

class Guard {
public:
    explicit Guard(double limit) : limitG(limit) {}

    Trip check(const Sample& sample) {
        if (!validAdc(sample)) return Trip::adcInvalid;
        if (initialized) {
            if (sample.hostUs <= lastHost || sample.hostUs - lastHost > maxGapUs) return Trip::hostGap;
            const Tick advance = sample.adcClock - lastClock;
            if (advance >= 0x80000000u || (!advance && sample.hostUs - lastAdvance > maxGapUs))
                return Trip::adcInvalid;
            if (advance) lastAdvance = sample.hostUs;
        } else {
            lastAdvance = sample.hostUs;
            initialized = true;
        }
        lastHost = sample.hostUs;
        lastClock = sample.adcClock;
        if (!std::isfinite(sample.following) || !std::isfinite(sample.planned) ||
            !std::isfinite(sample.position) || std::abs(sample.following) >= followLimit ||
            std::abs(sample.position - sample.planned) >= followLimit) return Trip::follow;
        return std::abs(sample.forceG) >= limitG ? Trip::force : Trip::none;
    }

private:
    double limitG;
    bool initialized = false;
    long long lastHost = 0, lastAdvance = 0;
    Tick lastClock = 0;
};

// The injected test and the hardware entry share this dispatch, including the
// exception path from an SDK failure during GTN_Update or a feedback read.
template<class Arm, class Poll, class Stop>
Trip protectedPress(Arm arm, Poll poll, Stop stop) {
    Trip reason = Trip::none;
    try {
        arm();
        while ((reason = poll()) == Trip::none) {}
    } catch (...) {
        stop();
        throw;
    }
    stop();
    return reason;
}

bool mappingHoldAt(const State& state, long target) {
    return state.atTailPosition(target) && std::abs(state.planV) <= velocityTol;
}

void selfTest() {
    State hold{};
    hold.x = 17672;
    hold.planX = 17670;
    hold.v = -20;
    require(mappingHoldAt(hold, 17670), "Isolated encoder velocity rejected a stationary hold");
    hold.status = movingMask;
    require(!mappingHoldAt(hold, 17670), "Motion flag accepted during hold");
    hold.status = 0;
    hold.planV = .1;
    require(!mappingHoldAt(hold, 17670), "Planner velocity accepted during hold");
    hold.planV = 0;
    hold.x = 17681;
    require(!mappingHoldAt(hold, 17670), "Encoder outside hold tolerance accepted");
    TailWindow stableHold;
    require(!stableHold.extend(0, 17670) && !stableHold.extend(15000, 17672) &&
            !stableHold.extend(30000, 17678) && stableHold.startUs == 30000,
            "Hold position window failed to detect displacement");
    std::cout << "Mapping hold: encoder spike, motion flag, planned velocity and position span checked\n";
    constexpr double measuredZeroV = -0.000517555165066614;
    constexpr double measuredReferenceV = 0.192470251579643;
    const auto measured = makeCalibration(measuredZeroV, measuredReferenceV, 19.1, 10);
    require(std::abs(measured.grams(measuredReferenceV) - 19.1) < 1e-9 &&
            measured.thresholdWithinAdcRange(10) &&
            measured.thresholdWithinAdcRange(mappingThresholdG) && !measured.thresholdWithinAdcRange(1000),
            "Measured calibration did not distinguish the pilot and formal thresholds");
    const auto reversed = makeCalibration(measuredZeroV, -measuredReferenceV, 19.1, 10);
    require(reversed.grams(-measuredReferenceV) > 19.0, "Reversed ADC polarity rejected");
    std::cout << "Measured reference: pilot and projected 200 g in range; projected 1000 g outside range\n";
    const auto check = [](Trip expected, auto mutate, double limit = 10.0) {
        Guard guard(limit);
        Sample first{1000, 100, 0, 0, 0, 15670, 15670, 0};
        Sample next{16000, 115, 0, .009, 9, 15675, 15675, 0};
        auto injected = next;
        injected.hostUs += 15000;
        injected.adcClock += 15;
        mutate(injected);
        const std::array<Sample, 3> samples{{first, next, injected}};
        std::size_t index = 0;
        int stops = 0, arms = 0;
        const auto result = protectedPress([&] { ++arms; },
                                           [&] { return guard.check(samples.at(index++)); },
                                           [&] { ++stops; });
        require(result == expected && arms == 1 && index == samples.size() && stops == 1,
                "Injected trip did not stop exactly once after two normal samples");
        std::cout << name(result) << ": stop dispatch verified\n";
    };
    check(Trip::force, [](Sample& s) { s.forceG = 10; });
    check(Trip::force, [](Sample& s) { s.forceG = -10; });
    check(Trip::force, [](Sample& s) { s.forceG = mappingThresholdG; }, mappingThresholdG);
    check(Trip::adcInvalid, [](Sample& s) { s.adcRc = -1; });
    check(Trip::adcInvalid, [](Sample& s) { s.volts = std::numeric_limits<double>::quiet_NaN(); });
    check(Trip::adcInvalid, [](Sample& s) { s.volts = 10; });
    check(Trip::follow, [](Sample& s) { s.following = -500; });
    check(Trip::follow, [](Sample& s) { s.planned += 500; });
    check(Trip::hostGap, [](Sample& s) { s.hostUs += maxGapUs; });

    Guard guard(10.0);
    Sample sample{1000, 0xffffffffu, 0, 0, 0, 15670, 15670, 0};
    require(guard.check(sample) == Trip::none, "Initial clock rejected");
    sample.hostUs += 15000;
    sample.adcClock = 0;
    require(guard.check(sample) == Trip::none, "ADC clock wrap rejected");
    sample.hostUs += 30000;
    require(guard.check(sample) == Trip::none, "Short repeat rejected");
    sample.hostUs += 21000;
    require(guard.check(sample) == Trip::adcInvalid, "Stale ADC clock accepted");

    for (const bool armFails : {true, false}) {
        int stops = 0;
        bool failed = false;
        try {
            protectedPress([armFails] { if (armFails) throw std::runtime_error("mock update failure"); },
                           []() -> Trip { throw std::runtime_error("mock feedback failure"); },
                           [&] { ++stops; });
        } catch (const std::runtime_error&) {
            failed = true;
        }
        require(failed && stops == 1, "SDK exception did not dispatch stop exactly once");
    }
    std::cout << "ADC clock wrap and staleness, update and feedback failure: verified\n";
}

Sample readForce(const Calibration& calibration) {
    Sample sample;
    sample.volts = std::numeric_limits<double>::quiet_NaN();
    sample.adcRc = GTN_GetAdc(core, adcChannel, &sample.volts, 1, &sample.adcClock);
    sample.hostUs = Axis::hostUs(Clock::now());
    sample.forceG = calibration.grams(sample.volts);
    return sample;
}

struct TraceRow {
    const char* phase;
    Sample sample;
    Trip trip;
};

int pilot(const Calibration& calibration, long safeUpper, double referenceV, double referenceGrams) {
    const auto root = projectRoot(L"ZRForceAcquisition.vcxproj");
    const auto id = sessionId("Z-FA0-PILOT_");
    auto log = createFile(root / L"manifest" / (id + ".txt"));
    auto raw = createFile(root / L"data/raw" / (id + ".force_guard.csv"));
    log << "session=" << id << "\nprotocol=" << protocol << "\nbuild=" << __DATE__ << ' ' << __TIME__
        << "\noperator_confirmation=PILOT\nstart_pulse=" << startPulse << "\ntarget_pulse=" << targetPulse
        << "\nsite_safe_upper_pulse=" << safeUpper << "\nspeed_mm_s=" << speed
        << "\nacceleration_mm_s2=" << acceleration << "\nrequested_follow_pulse=" << followLimit
        << "\ntest_threshold_g=" << calibration.tripGrams
        << "\nnext_batch_requested_threshold_g=" << mappingThresholdG
        << "\nnext_batch_threshold_within_projected_adc_range="
        << calibration.thresholdWithinAdcRange(mappingThresholdG)
        << "\nzero_v=" << calibration.zeroV << "\nreference_v=" << referenceV
        << "\nreference_load_g=" << referenceGrams << "\nscale_g_per_v=" << calibration.gramsPerV
        << "\nmax_monitored_gap_us=" << maxGapUs << '\n' << std::flush;

    Recorder card(log);
    Axis motor(card, log);
    Guard guard(calibration.tripGrams);
    std::vector<TraceRow> trace;
    trace.reserve(4096);
    auto check = [&](const char* phase, bool stationary) {
        require(trace.size() < trace.capacity(), "monitor_buffer_exhausted");
        const auto state = motor.feedback();
        auto sample = readForce(calibration);
        sample.position = state.x;
        sample.planned = state.planX;
        sample.following = state.error;
        const auto trip = guard.check(sample);
        trace.push_back({phase, sample, trip});
        if (trip == Trip::none) {
            motor.validate(state);
            if (stationary) {
                require(state.at(startPulse) && std::abs(state.planX - startPulse) <= positionTol * pulsePerMm &&
                        std::abs(state.planV) <= velocityTol, "expected_stationary_unloaded_start");
                require(std::abs(sample.forceG) < calibration.tripGrams / 2, "unloaded_force_too_close_to_trip");
            }
        }
        return trip;
    };

    std::string outcome = "incomplete";
    bool stopConfirmed = false, stopSampleLost = false;
    Trip reason = Trip::none;
    long long stopBegin = 0, stopEnd = 0;
    auto recordStopSample = [&](const char* phase) {
        if (trace.size() == trace.capacity()) {
            stopSampleLost = true;
            return;
        }
        auto sample = readForce(calibration);
        sample.position = sample.planned = sample.following = std::numeric_limits<double>::quiet_NaN();
        if (!validAdc(sample) || (!trace.empty() && sample.hostUs - trace.back().sample.hostUs > maxGapUs))
            stopSampleLost = true;
        trace.push_back({phase, sample, Trip::none});
    };
    const std::function<void()> observeBraking = [&] { recordStopSample("braking"); };
    auto stop = [&] {
        stopBegin = Axis::hostUs(Clock::now());
        stopConfirmed = motor.abort(log, observeBraking);
        stopEnd = Axis::hostUs(Clock::now());
    };

    try {
        require(!stopRequested(), "operator_stop");
        card.connect();
        const auto tick = verifyCorePeriod(card, log);
        motor.setFollowLimit(followLimit);
        double smooth = 0, abrupt = 0;
        card.check("GTN_GetStopDec", GTN_GetStopDec(core, axis, &smooth, &abrupt));
        const double smoothMmS2 = smooth * 1e6 / pulsePerMm;
        log << "smooth_stop_dec_pulse_ms2=" << smooth << "\nabrupt_stop_dec_pulse_ms2=" << abrupt << '\n';
        require(std::isfinite(smoothMmS2) && smoothMmS2 > 0 && std::isfinite(abrupt) && abrupt > 0,
                "invalid_stop_deceleration");
        const double reserve = followLimit / pulsePerMm + speed * maxGapUs / 1e6;
        require(pathCheck(log, "Force guard pilot", mm(startPulse), mm(targetPulse), speed,
                          reserve, smoothMmS2), "gross_travel_reserve_insufficient");
        const double localMaximum = mm(targetPulse) + reserve + positionTol +
                                    speed * speed / (2 * smoothMmS2);
        log << "calculated_positive_envelope_pulse=" << localMaximum * pulsePerMm << '\n';
        require(std::isfinite(localMaximum) && localMaximum <= mm(safeUpper),
                "site_positive_stop_reserve_insufficient");

        motor.readyAt(startPulse, &log, "preflight");
        const auto until = Clock::now() + 250ms;
        while (Clock::now() < until) {
            require(check("baseline", true) == Trip::none, "baseline_guard_failed");
            std::this_thread::sleep_for(pollWait);
        }
        motor.setTrap(targetPulse, speed, acceleration, log, "pilot", tick);
        motor.readyAt(startPulse, &log, "before_update");
        require(!stopRequested(), "operator_stop");
        guard = Guard(calibration.tripGrams);
        require(check("armed", true) == Trip::none, "armed_guard_failed");
        const auto deadline = Clock::now() + 4s;
        double furthest = startPulse;
        reason = protectedPress([&] { motor.issue(); }, [&] {
            if (stopRequested()) return Trip::operatorStop;
            if (Clock::now() >= deadline) return Trip::timeout;
            const auto trip = check("press", false);
            if (trip != Trip::none) return trip;
            const double position = trace.back().sample.position;
            require(position >= furthest - positionTol * pulsePerMm &&
                    position <= targetPulse + positionTol * pulsePerMm,
                    "unexpected_motion_direction_or_overshoot");
            furthest = std::max(furthest, position);
            if (position >= targetPulse - positionTol * pulsePerMm) return Trip::target;
            std::this_thread::sleep_for(pollWait);
            return Trip::none;
        }, stop);

        const auto finalState = motor.read();
        log << "stop_position_pulse=" << finalState.x << "\nstop_planned_pulse=" << finalState.planX
            << "\nstop_velocity_sdk=" << finalState.v << '\n';
        require(stopConfirmed && finalState.idle() && std::abs(finalState.planV) <= velocityTol &&
                finalState.x >= startPulse - positionTol * pulsePerMm && finalState.x <= safeUpper,
                "stationary_stop_or_position_not_confirmed");
        const auto postUntil = Clock::now() + 250ms;
        while (Clock::now() < postUntil) {
            recordStopSample("post_stop");
            std::this_thread::sleep_for(pollWait);
        }
        const auto trigger = std::find_if(trace.begin(), trace.end(),
            [](const TraceRow& row) { return row.trip == Trip::force; });
        if (reason == Trip::force && trigger != trace.end()) {
            log << "trigger_force_g=" << trigger->sample.forceG
                << "\ntrigger_position_pulse=" << trigger->sample.position
                << "\ntrigger_to_stop_call_us=" << stopBegin - trigger->sample.hostUs << '\n';
            require(trigger->sample.forceG >= calibration.tripGrams &&
                    trigger->sample.position > startPulse &&
                    trigger->sample.position < targetPulse - positionTol * pulsePerMm &&
                    !stopSampleLost, "force_trip_or_braking_samples_require_review");
            outcome = "pilot_stop_observed_pending_independent_verification";
        }
    } catch (const std::exception& error) {
        log << "error=" << error.what() << '\n';
        std::cerr << error.what() << '\n';
    }
    card.cleanup();

    raw << "phase,host_us,adc_clock,adc_rc,voltage_v,force_g,position_pulse,planned_pulse,following_pulse,trip\n";
    double peak = 0;
    for (const auto& row : trace) {
        const auto& sample = row.sample;
        raw << row.phase << ',' << sample.hostUs << ',' << sample.adcClock << ',' << sample.adcRc
            << ',' << sample.volts << ',' << sample.forceG << ',' << sample.position << ',' << sample.planned
            << ',' << sample.following << ',' << name(row.trip) << '\n';
        if (validAdc(sample)) peak = std::max(peak, std::abs(sample.forceG));
    }
    raw.flush();
    if (!card.cleanupOk) outcome = "cleanup_failed";
    log << "trip_reason=" << name(reason) << "\npeak_sampled_abs_g=" << peak
        << "\nstop_call_and_confirmation_us=" << (stopEnd - stopBegin)
        << "\nstop_samples_lost=" << stopSampleLost << "\noutcome=" << outcome << '\n' << std::flush;
    std::cout << id << ' ' << outcome << ' ' << name(reason) << '\n';
    return outcome == "pilot_stop_observed_pending_independent_verification" ? 0 : 1;
}

constexpr std::array<long, 5> mappingPoints{{startPulse, 16670, 17670, 18670, startPulse}};
constexpr auto mappingHold = 3s;

struct MappingRow {
    std::size_t point;
    const char* phase;
    Sample sample;
    short current = 0;
    short currentRc = -1;
    long long currentHostUs = 0;
    double velocitySdk = 0, plannedVelocitySdk = 0;
    long axisStatus = 0;
};

int mappingRun(const Calibration& calibration, long safeUpper, bool loaded) {
    const auto root = projectRoot(L"ZRForceAcquisition.vcxproj");
    const auto id = sessionId("Z-FA0-MAP_");
    auto log = createFile(root / L"manifest" / (id + ".txt"));
    auto raw = createFile(root / L"data/raw" / (id + ".current_force.csv"));
    log << "session=" << id << "\nprotocol=Z-FA0-SPRING-01-MAP-200"
        << "\nbuild=" << __DATE__ << ' ' << __TIME__
        << "\ncondition=" << (loaded ? "loaded" : "unloaded")
        << "\noperator_confirmation=MAP\nforce_threshold_g=" << calibration.tripGrams
        << "\nrequested_follow_pulse=" << followLimit << "\nsite_safe_upper_pulse=" << safeUpper
        << "\nspeed_mm_s=" << speed << "\nacceleration_mm_s2=" << acceleration
        << "\nhold_s=" << mappingHold.count() << "\nzero_v=" << calibration.zeroV
        << "\nscale_display_g_per_v=" << calibration.gramsPerV << "\npoint_table_pulse=";
    for (const long target : mappingPoints) log << target << ' ';
    log << '\n' << std::flush;

    Recorder card(log);
    Axis motor(card, log);
    Guard guard(calibration.tripGrams);
    std::vector<MappingRow> trace;
    trace.reserve(16000);
    auto read = [&](std::size_t point, const char* phase) {
        require(!stopRequested(), "operator_stop");
        require(trace.size() < trace.capacity(), "monitor_buffer_exhausted");
        const auto state = motor.feedback();
        auto sample = readForce(calibration);
        sample.position = state.x;
        sample.planned = state.planX;
        sample.following = state.error;
        trace.push_back({point, phase, sample, 0, -1, 0, state.v, state.planV, state.status});
        const auto trip = guard.check(sample);
        require(trip == Trip::none, name(trip));
        motor.validate(state);
        auto& row = trace.back();
        row.currentRc = GTN_GetAtlTorque(core, axis, &row.current);
        row.currentHostUs = Axis::hostUs(Clock::now());
        require(row.currentRc == 0, "current_feedback_read_failed");
        return state;
    };

    bool complete = false;
    try {
        require(!stopRequested(), "operator_stop");
        card.connect();
        const auto tick = verifyCorePeriod(card, log);
        motor.setFollowLimit(followLimit);
        double smooth = 0, abrupt = 0;
        card.check("GTN_GetStopDec", GTN_GetStopDec(core, axis, &smooth, &abrupt));
        const double dec = smooth * 1e6 / pulsePerMm;
        require(std::isfinite(dec) && dec > 0 && std::isfinite(abrupt) && abrupt > 0,
                "invalid_stop_deceleration");
        const double margin = followLimit / pulsePerMm + speed * maxGapUs / 1e6;
        const double localMaximum = mm(mappingPoints[3]) + margin + positionTol + speed * speed / (2 * dec);
        log << "positive_envelope_pulse=" << localMaximum * pulsePerMm
            << "\nsmooth_stop_dec_pulse_ms2=" << smooth << "\nabrupt_stop_dec_pulse_ms2=" << abrupt << '\n';
        require(pathCheck(log, "Mapping", mm(startPulse), mm(mappingPoints[3]), speed, margin, dec) &&
                std::isfinite(localMaximum) && localMaximum <= mm(safeUpper),
                "mapping_stop_reserve_insufficient");
        motor.readyAt(startPulse, nullptr, "");
        long previous = startPulse;
        for (std::size_t point = 0; point < mappingPoints.size(); ++point) {
            const long target = mappingPoints[point];
            const double tolerancePulse = positionTol * pulsePerMm;

            if (target != previous) {
                require(mappingHoldAt(read(point, "pre_move"), previous), "not_at_confirmed_segment_start");
                motor.setTrap(target, speed, acceleration, log, "map", tick);
                require(mappingHoldAt(read(point, "armed"), previous), "not_stationary_before_update");
                motor.issue();
                const auto deadline = Clock::now() +
                    std::chrono::duration<double>(std::abs(mm(target - previous)) / speed + 2);
                const int direction = target > previous ? 1 : -1;
                double furthest = 0;
                TailWindow settled;
                bool accepted = false;
                while (Clock::now() < deadline) {
                    const auto state = read(point, "move");
                    const double progress = direction * (state.x - previous);
                    require(progress >= furthest - tolerancePulse &&
                            progress <= std::abs(target - previous) + tolerancePulse,
                            "unexpected_motion_direction_or_overshoot");
                    furthest = std::max(furthest, progress);
                    if (mappingHoldAt(state, target)) {
                        if (settled.extend(trace.back().sample.hostUs, state.x)) {
                            accepted = true;
                            break;
                        }
                    } else {
                        settled = TailWindow{};
                    }
                    std::this_thread::sleep_for(pollWait);
                }
                require(accepted, "mapping_arrival_timeout");
            }
            TailWindow holdPosition;
            const auto holdUntil = Clock::now() + mappingHold;
            do {
                if (Clock::now() < holdUntil) std::this_thread::sleep_for(pollWait);
                const auto state = read(point, "hold");
                require(mappingHoldAt(state, target), "mapping_hold_motion_flag_plan_or_position");
                const bool tracking = holdPosition.open;
                const auto windowStart = holdPosition.startUs;
                holdPosition.extend(trace.back().sample.hostUs, state.x);
                require(!tracking || holdPosition.startUs == windowStart, "mapping_hold_position_span_or_gap");
                if (!loaded || point == 0 || point == mappingPoints.size() - 1) {
                    require(std::abs(trace.back().sample.forceG) < 5, "expected_unloaded_force_during_hold");
                }
            } while (Clock::now() < holdUntil);
            previous = target;
        }
        require(motor.abort(log), "final_stationary_stop_not_confirmed");
        complete = true;
    } catch (const std::exception& error) {
        motor.abort(log);
        log << "error=" << error.what() << '\n';
        std::cerr << error.what() << '\n';
    }

    card.cleanup();
    raw << "point,target_pulse,phase,adc_host_us,adc_clock,voltage_v,force_display_g,adc_rc,position_pulse,"
           "planned_pulse,following_pulse,current_raw,current_rc,current_host_us,velocity_sdk,planned_velocity_sdk,axis_status\n";
    for (const auto& row : trace) {
        const auto& sample = row.sample;
        raw << row.point << ',' << mappingPoints[row.point] << ',' << row.phase << ',' << sample.hostUs
            << ',' << sample.adcClock << ',' << sample.volts << ',' << sample.forceG << ',' << sample.adcRc
            << ',' << sample.position << ',' << sample.planned << ',' << sample.following << ','
            << row.current << ',' << row.currentRc << ',' << row.currentHostUs << ','
            << row.velocitySdk << ',' << row.plannedVelocitySdk << ',' << row.axisStatus << '\n';
    }
    raw.flush();
    complete = complete && card.cleanupOk;
    log << "outcome=" << (complete ? "mapping_complete_pending_independent_review" : "incomplete")
        << '\n' << std::flush;
    std::cout << id << (complete ? " mapping_complete_pending_independent_review" : " incomplete") << '\n';
    return complete ? 0 : 1;
}

bool takeLine(std::string& input) {
    return std::getline(std::cin, input) && input != "Q" && input != "q";
}
}  // namespace

int main(int argc, char** argv) {
    // The VS project emits UTF-8 string literals. Match the attached Windows console.
    DWORD consoleMode = 0;
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &consoleMode)) {
        SetConsoleOutputCP(CP_UTF8);
    }
    try {
        const std::string option = argc == 2 ? argv[1] : "";
        if (option == "--self-test") {
            selfTest();
            return 0;
        }
        if (option == "--plan") {
            std::cout << protocol << ": one 15670 -> 16670 pulse press, 0.5 mm/s, 20 mm/s^2, "
                         "500 pulse follow limit, temporary 7-15 g. No automatic return. Offline.\n";
            return 0;
        }
        if (option == "--mapping-plan") {
            std::cout << "Z-FA0-SPRING-01-MAP-200: 15670 -> 16670 -> 17670 -> 18670 -> 15670 pulse, "
                         "3 s holds, 0.5 mm/s, 20 mm/s^2, 200 g, 500 pulse. Offline.\n";
            return 0;
        }

        const bool mapping = option == "--mapping";
        require(argc == 1 || mapping,
                "Usage: ZRForceGuardPilot.exe [--plan | --self-test | --mapping-plan | --mapping]");
        std::cout << (mapping ? "Z-FA0-SPRING-01-MAP-200，要求浅压首件及独立停轴复核已通过。\n"
                             : "Z-FA0-SPRING-01-PILOT，现场首件，只执行一次正向浅压 15670 -> 16670 pulse。\n")
                  << "输入零载电压 V、参考电压 V、对应的仪表示值或独立参考载荷 g，逗号分隔 > ";
        std::string input;
        if (!takeLine(input)) return 0;
        const auto values = numbers(input, 3);

        double pilotLimit = 10;
        bool loaded = false;
        if (mapping) {
            std::cout << "样件状态，带载填 1，移除弹簧的空载对照填 0 > ";
            if (!takeLine(input)) return 0;
            require(input == "0" || input == "1", "Invalid specimen condition");
            loaded = input == "1";
        } else {
            std::cout << "首件测试阈值 7–15 g，回车采用 10 g > ";
            if (!takeLine(input)) return 0;
            if (!input.empty()) pilotLimit = parseNumber(input);
            require(pilotLimit >= 7 && pilotLimit <= 15, "Pilot trip threshold must be 7-15 g");
        }
        auto calibration = makeCalibration(values[0], values[1], values[2], pilotLimit);
        if (mapping) {
            require(calibration.thresholdWithinAdcRange(mappingThresholdG),
                    "Projected 200 g threshold outside usable ADC range");
            calibration.tripGrams = mappingThresholdG;
        }

        std::cout << "输入现场已确认含停车余量的正向安全上界 pulse > ";
        if (!takeLine(input)) return 0;
        const double safeUpper = parseNumber(input);
        const auto farthest = mapping ? mappingPoints[3] : targetPulse;
        require(safeUpper > farthest && safeUpper <= upper * pulsePerMm &&
                safeUpper == std::trunc(safeUpper), "Invalid site positive boundary");
        std::cout << std::setprecision(9) << "试验阈值 " << calibration.tripGrams << " g，跟随误差 500 pulse，"
                  << "换算 " << calibration.gramsPerV << " g/V，安全上界 " << safeUpper << " pulse。\n";
        const char* confirmation = mapping ? "MAP" : "PILOT";
        if (mapping) std::cout << "确认首件、批前读卡、点表、安全上界和当前样件状态已核对。\n";
        std::cout << "现场确认急停、R 固定、抱闸和停车条件后，输入 " << confirmation << " > ";
        if (!takeLine(input) || input != confirmation) return 0;
        require(SetConsoleCtrlHandler(onCtrl, TRUE), "Cannot install console cancellation handler");
        return mapping ? mappingRun(calibration, static_cast<long>(safeUpper), loaded) :
                         pilot(calibration, static_cast<long>(safeUpper), values[1], values[2]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
