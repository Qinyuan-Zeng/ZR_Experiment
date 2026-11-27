#pragma once

#include "watch_capture.hpp"
#include <functional>

namespace matched_motion {
using namespace watch_capture;

constexpr double pulsePerMm = 1000, lower = 0, upper = 32;
constexpr double positionTol = 0.010, velocityTol = 0.05;
constexpr double transferSpeed = 2, transferAcc = 20;
constexpr double transferAccRequest = transferAcc * pulsePerMm / 1e6;
// Observed on core 2 at 50 us. Transfer and measurement readbacks truncate
// to this acceleration step, in pulse/ms^2.
constexpr double accelerationStep50Us = 0.006103515625;
constexpr double observedTransferAcc = 0.018310546875;
constexpr double baselineMs = 100, tailMs = 150;
constexpr double tailSpanPulse = velocityTol * pulsePerMm * tailMs / 1000;
constexpr long long tailConfirmUs = static_cast<long long>(tailMs * 1000), maxTailPollGapUs = 50000;
constexpr long axisMask = 1L << (axis - 1);
constexpr long enableMask = 1L << 9, movingMask = 1L << 10;
constexpr long faultMask = (1L << 1) | (1L << 5) | (1L << 6);

constexpr bool accelerationMatches(double requested, double actual, Ns coreTick) {
    const double tolerance = requested * .01;
    if (actual >= requested - tolerance && actual <= requested + tolerance) return true;
    if (coreTick != 50000 || requested < accelerationStep50Us || requested > 0.8) return false;
    const auto steps = static_cast<int>(requested / accelerationStep50Us);
    return actual == steps * accelerationStep50Us;
}

static_assert(accelerationMatches(transferAccRequest, observedTransferAcc, 50000));
static_assert(!accelerationMatches(transferAccRequest, observedTransferAcc, 0));
static_assert(!accelerationMatches(transferAccRequest, observedTransferAcc, 100000));
static_assert(!accelerationMatches(transferAccRequest, 0.026, 50000));
static_assert(!accelerationMatches(0.4, observedTransferAcc, 50000));
static_assert(accelerationMatches(0.2, 0.1953125, 50000));
static_assert(!accelerationMatches(0.2, 0.1953125, 100000));
static_assert(!accelerationMatches(0.2, 0.189208984375, 50000));
static_assert(!accelerationMatches(0.2, 0.201416015625 + 0.006103515625, 50000));

struct Recipe {
    double planCenter = 16, acc = 400, peak = 40, match = 20;
    int direction = 1, pairs = 1;
    double evalCenter = 15.976;
    int firstRoleSign = 1;
};

struct Run {
    const char* role;
    long start, end;
    double matchSec, platformBegin, platformEnd, duration, matchSpeed, evalSec;
};

struct Plan {
    Recipe recipe;
    Run acceleration, deceleration;
};

inline long pulse(double mm) { return static_cast<long>(std::lround(mm * pulsePerMm)); }
inline double mm(double ticks) { return ticks / pulsePerMm; }

inline Run timingFor(const Recipe& recipe, const char* role, long from, long to, double acc, double dec) {
    const bool accelerating = role[0] == 'a';
    const int sign = recipe.direction;
    const double distance = sign * mm(to - from);
    require(distance > 0 && acc > 0 && dec > 0, "Invalid TRAP distance or acceleration");
    const double inverse = 1 / acc + 1 / dec;
    const double peak = std::min(recipe.peak, std::sqrt(2 * distance / inverse));
    const double ramp = peak / acc, fall = peak / dec;
    const double cruise = std::max(0.0, (distance - peak * (ramp + fall) / 2) / peak);
    const double duration = ramp + cruise + fall;
    const auto distanceAt = [&](double center) {
        return accelerating ? sign * (center - mm(from)) : sign * (mm(to) - center);
    };
    const double remaining = distanceAt(recipe.planCenter);
    const double localAcc = accelerating ? acc : dec;
    require(duration <= 3 && remaining > 0 && remaining < peak * peak / (2 * localAcc),
            "Match is outside the constant-acceleration platform or arrival budget");
    const double speed = std::sqrt(2 * localAcc * remaining);
    const double t = accelerating ? speed / acc : duration - speed / dec;
    const double begin = accelerating ? 0 : ramp + cruise;
    const double end = accelerating ? ramp : duration;
    require(std::min(t - begin, end - t) > .0155 && std::abs(speed - recipe.match) <= 0.2,
            "TRAP cannot fit the 31 ms window or match target speed");
    const double evalDistance = distanceAt(recipe.evalCenter);
    require(evalDistance > 0 && evalDistance < peak * peak / (2 * localAcc),
            "Evaluation point is outside the role's planned acceleration platform");
    const double evalFromEndpointSec = std::sqrt(2 * evalDistance / localAcc);
    const double evalSec = accelerating ? evalFromEndpointSec : duration - evalFromEndpointSec;
    require(std::min(evalSec - begin, end - evalSec) > .0155,
            "Evaluation point cannot fit the full 31 ms planned window");
    return {role, from, to, t, begin, end, duration, speed, evalSec};
}

inline Plan makePlan(Recipe recipe, double maximumAcceleration = 400) {
    require(recipe.planCenter >= 1 && recipe.planCenter <= 31 &&
                recipe.evalCenter >= 1 && recipe.evalCenter <= 31 && recipe.acc > 0 &&
                recipe.acc <= maximumAcceleration && recipe.peak > recipe.match && recipe.peak <= 40 &&
                recipe.match > 0 && (recipe.direction == 1 || recipe.direction == -1),
            "Positions or velocity/acceleration are outside the entry's planning limits");
    require(mm(pulse(recipe.planCenter)) == recipe.planCenter &&
                mm(pulse(recipe.evalCenter)) == recipe.evalCenter,
            "Both positions must lie on the encoder pulse grid");
    const double acc = recipe.acc;
    const double toMatch = recipe.match * recipe.match / (2 * acc);
    const double distance = recipe.peak * recipe.peak / acc;
    require(std::isfinite(distance) && distance <= upper - lower,
            "Ideal trajectory exceeds working travel");
    const int sign = recipe.direction;
    const auto makeRun = [&](bool accelerating) -> Run {
        const double beforeMatch = accelerating ? toMatch : distance - toMatch;
        const long from = pulse(recipe.planCenter - sign * beforeMatch);
        const long to = pulse(recipe.planCenter + sign * (distance - beforeMatch));
        require(mm(from) >= lower && mm(from) <= upper && mm(to) >= lower && mm(to) <= upper,
                "Rounded trajectory endpoint outside 0..32 mm");
        return timingFor(recipe, accelerating ? "accel" : "decel", from, to, acc, acc);
    };
    return {recipe, makeRun(true), makeRun(false)};
}

inline bool pathCheck(std::ostream& out, const char* name, double from, double to,
               double speed, double margin, double smoothStop) {
    const double lo = std::min(from, to), hi = std::max(from, to);
    out << name << ' ' << from << " -> " << to << " mm, envelope [" << lo << ", " << hi
        << "], boundary clearances " << lo - lower << '/' << upper - hi << " mm";
    const bool inRange = lo >= lower && hi <= upper;
    if (margin > 0 && smoothStop > 0) {
        const double reserve = margin + positionTol + speed * speed / (2 * smoothStop);
        out << ", required reserve " << reserve << " mm each side";
        const bool enough = inRange && lo - lower >= reserve && upper - hi >= reserve;
        out << (enough ? " [OK]\n" : " [FAIL]\n");
        return enough;
    }
    out << (inRange ? " [theoretical only]\n" : " [FAIL]\n");
    return inRange;
}

inline bool printPlan(std::ostream& out, const Plan& plan, double initial, double margin, double smoothStop,
                      const char* label = "A1") {
    const auto& p = plan.recipe;
    out << label << " zero-smoothing symmetric TRAP, x0_plan=" << p.planCenter
        << " mm, x_match_eval=" << p.evalCenter << " mm, A=" << p.acc
        << " mm/s^2, V=" << p.peak << ", v0=" << p.match << " mm/s, direction=" << p.direction
        << ", pairs=" << p.pairs << ", first=" << (p.firstRoleSign > 0 ? "accel" : "decel") << "\n"
        << "Field confirmation: " << (margin > 0 ? "reserve calculation from provided inputs" : "pending, theoretical only")
        << "\nInitial position for transfer check: " << initial << " mm\n";
    bool ok = true;
    double position = initial;
    for (int pair = 1; pair <= p.pairs; ++pair) {
        out << "Pair " << pair << '\n';
        const bool accelFirst = (pair % 2 != 0) == (p.firstRoleSign > 0);
        for (const Run* run : {accelFirst ? &plan.acceleration : &plan.deceleration,
                               accelFirst ? &plan.deceleration : &plan.acceleration}) {
            ok = pathCheck(out, "Transfer", position, mm(run->start), transferSpeed, margin, smoothStop) && ok;
            ok = pathCheck(out, run->role, mm(run->start), mm(run->end), p.peak, margin, smoothStop) && ok;
            out << "  match speed=" << p.direction * run->matchSpeed << " mm/s, signed a="
                << (run->role[0] == 'a' ? p.direction : -p.direction) * p.acc
                << " mm/s^2, platform before/after=" << 1000 * (run->matchSec - run->platformBegin)
                << '/' << 1000 * (run->platformEnd - run->matchSec) << " ms, duration="
                << 1000 * run->duration << " ms\n";
            out << "  evaluation platform before/after="
                << 1000 * (run->evalSec - run->platformBegin) << '/'
                << 1000 * (run->platformEnd - run->evalSec) << " ms\n";
            position = mm(run->end);
        }
    }
    return ok;
}

struct State {
    double x = 0, v = 0, planX = 0, planV = 0, error = 0;
    unsigned long planXClock = 0, planVClock = 0, errorClock = 0, encoderClock = 0;
    long status = 0, planner = 0;
    short mode = 0, plus = 0, minus = 0;
    bool idle() const { return !(status & movingMask) && std::abs(v) <= velocityTol; }
    bool at(long target) const { return idle() && std::abs(mm(x - target)) <= positionTol; }
    constexpr bool atTailPosition(long target) const {
        const double tolerance = positionTol * pulsePerMm;
        return !(status & movingMask) && x >= target - tolerance && x <= target + tolerance &&
               planX >= target - tolerance && planX <= target + tolerance;
    }
};

struct TailWindow {
    long long startUs = 0, previousUs = 0;
    double minimum = 0, maximum = 0;
    bool open = false;

    constexpr bool extend(long long now, double pulse) {
        if (!open || now - previousUs > maxTailPollGapUs ||
            std::max(maximum, pulse) - std::min(minimum, pulse) > tailSpanPulse) {
            startUs = now;
            minimum = maximum = pulse;
            open = true;
        }
        previousUs = now;
        minimum = std::min(minimum, pulse);
        maximum = std::max(maximum, pulse);
        return now - startUs >= tailConfirmUs;
    }
};

constexpr bool verifyTailWindow() {
    State atEnd{};
    atEnd.x = 16492;
    atEnd.planX = 16500;
    atEnd.v = -20;  // Real isolated SDK reading at stall of the old check.
    if (!atEnd.atTailPosition(16500)) return false;
    atEnd.status = movingMask;
    if (atEnd.atTailPosition(16500)) return false;
    TailWindow stable, drifting, late;
    for (long long tick = 0; tick <= 150000; tick += 15000) {
        if (stable.extend(tick, 16492 + double(tick / 50000)) != (tick == 150000)) return false;
        if (drifting.extend(tick, 16490 + double(tick / 15000))) return false;
    }
    return !late.extend(0, 16500) && !late.extend(150000, 16500);
}
static_assert(verifyTailWindow(), "Tail requires stable position and contiguous polls");

class Axis {
public:
    using Monitor = std::function<void(const State&)>;
    Axis(Recorder& connection, std::ostream& manifest, Monitor observe = {})
        : card(connection), log(manifest), monitor(std::move(observe)) {}
    ~Axis() { abort(log); }

    State feedback(std::ostream* trace = nullptr, const char* phase = "") {
        State s;
        const auto begin = Clock::now();
        const short rx = GTN_GetAxisEncPos(core, axis, &s.x, 1, monitor ? &s.encoderClock : nullptr);
        const short rv = GTN_GetAxisEncVel(core, axis, &s.v);
        const short rs = GTN_GetSts(core, axis, &s.status);
        const short rm = GTN_GetControlLoopMode(core, axis, 1, &s.mode);
        const short rp = GTN_GetPrfMode(core, axis, &s.planner);
        const short rl = GTN_GetLimitStatus(core, axis, &s.plus, &s.minus);
        const short rpx = GTN_GetPrfPos(core, axis, &s.planX, 1, &s.planXClock);
        const short rpv = GTN_GetPrfVel(core, axis, &s.planV, 1, &s.planVClock);
        const short re = GTN_GetAxisError(core, axis, &s.error, 1, &s.errorClock);
        if (trace) {
            *trace << phase << ',' << hostUs(begin) << ',' << hostUs(Clock::now())
                   << ',' << s.x << ',' << s.v << ',' << s.status << ',' << s.planX << ','
                   << s.planXClock << ',' << s.planV << ',' << s.planVClock << ',' << s.error
                   << ',' << s.errorClock << ',' << rx << ',' << rv << ',' << rs << ','
                   << rm << ',' << rp << ',' << rl << ',' << rpx << ',' << rpv << ',' << re << '\n';
        }
        if (rx || rv || rs || rm || rp || rl || rpx || rpv || re) {
            log << "feedback_rc=" << rx << ',' << rv << ',' << rs << ',' << rm << ',' << rp
                << ',' << rl << ',' << rpx << ',' << rpv << ',' << re << '\n';
            throw std::runtime_error("missing_sdk_feedback");
        }
        return s;
    }

    void validate(const State& s) const {
        require((s.status & enableMask) && !(s.status & faultMask) && !s.plus && !s.minus,
                "axis_disabled_fault_or_limit");
        require(s.mode == 2 && s.planner == PROFILE_MODE_TRAP, "expected_position_servo_and_TRAP");
        require(std::isfinite(s.x) && std::isfinite(s.v) && std::isfinite(s.planX) &&
                std::isfinite(s.planV) && std::isfinite(s.error), "nonfinite_sdk_feedback");
        require(mm(s.x) >= lower && mm(s.x) <= upper, "axis_feedback_outside_0_32_mm");
        require(std::abs(s.error) <= followLimit && std::abs(s.x - s.planX) <= followLimit,
                "following_error_exceeds_limit");
    }

    State read(std::ostream* trace = nullptr, const char* phase = "") {
        const State s = feedback(trace, phase);
        validate(s);
        if (monitor) monitor(s);
        return s;
    }

    void setFollowLimit(double requestedPulse) {
        long configured = 0;
        card.check("GTN_GetAxisPosErrLimit", GTN_GetAxisPosErrLimit(core, axis, &configured));
        require(configured >= 0, "invalid_drive_following_error_limit");
        followLimit = configured > 0 ? std::min(double(configured), requestedPulse) : requestedPulse;
        log << "controller_follow_limit_pulse=" << configured
            << "\nfollow_limit_pulse=" << followLimit << '\n';
    }

    std::pair<TTrapPrm, double> setTrap(long target, double speed, double acceleration,
                                       std::ostream& record, const char* prefix, Ns coreTick) {
        TTrapPrm trap{}, actual{};
        trap.acc = trap.dec = acceleration * pulsePerMm / 1e6;
        card.check("GTN_SetTrapPrm", GTN_SetTrapPrm(core, axis, &trap));
        card.check("GTN_GetTrapPrm", GTN_GetTrapPrm(core, axis, &actual));
        record << prefix << "_trap_requested_acc_pulse_ms2=" << trap.acc << '\n'
               << prefix << "_trap_acc_pulse_ms2=" << actual.acc << '\n'
               << prefix << "_trap_dec_pulse_ms2=" << actual.dec << '\n'
               << prefix << "_trap_vel_start_pulse_ms=" << actual.velStart << '\n'
               << prefix << "_trap_smooth_ms=" << actual.smoothTime << '\n';
        require(actual.smoothTime == 0 && std::abs(actual.velStart) <= 1e-9 &&
                accelerationMatches(trap.acc, actual.acc, coreTick) &&
                accelerationMatches(trap.dec, actual.dec, coreTick),
                "TRAP_readback_differs_from_zero_smoothing_plan");
        card.check("GTN_SetPos", GTN_SetPos(core, axis, target));
        card.check("GTN_SetVel", GTN_SetVel(core, axis, speed * pulsePerMm / 1000));
        long actualTarget = 0;
        double actualVelocity = 0;
        card.check("GTN_GetPos", GTN_GetPos(core, axis, &actualTarget));
        card.check("GTN_GetVel", GTN_GetVel(core, axis, &actualVelocity));
        record << prefix << "_target_readback_pulse=" << actualTarget << '\n'
               << prefix << "_speed_readback_pulse_ms=" << actualVelocity << '\n';
        require(actualTarget == target && std::isfinite(actualVelocity) && actualVelocity > 0 &&
                std::abs(actualVelocity - speed * pulsePerMm / 1000) <= .01,
                "TRAP_target_or_speed_readback_mismatch");
        return {actual, actualVelocity * 1000 / pulsePerMm};
    }

    void readyAt(long start, std::ostream* trace, const char* phase) {
        require(!stopRequested(), "operator_stop");
        const auto s = read(trace, phase);
        require(s.at(start) && std::abs(mm(s.planX - start)) <= positionTol, "not_at_confirmed_start");
    }

    void issue() {
        require(!stopRequested(), "operator_stop");
        active = true;
        card.check("GTN_Update", GTN_Update(core, axisMask));
    }

    void settle(long from, long target, double limitSec, std::ostream* trace, const char* phase) {
        const auto until = Clock::now() + std::chrono::duration<double>(limitSec);
        const int dir = target > from ? 1 : -1;
        double furthest = 0;
        int settled = 0;
        while (Clock::now() < until) {
            require(!stopRequested(), "operator_stop");
            const auto s = read(trace, phase);
            const double progress = dir * mm(s.x - from);
            require(progress >= furthest - positionTol && progress <= std::abs(mm(target - from)) + positionTol,
                    "unexpected_motion_direction_or_target_overshoot");
            furthest = std::max(furthest, progress);
            settled = s.at(target) ? settled + 1 : 0;
            if (settled >= 3) {
                active = false;
                return;
            }
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error("arrival_timeout");
    }

    void abort(std::ostream& record) noexcept {
        if (!active) return;
        active = false;
        const short rc = GTN_Stop(core, axisMask, 0);
        bool stopped = false;
        const auto until = Clock::now() + 2s;
        do {
            long status = 0;
            double velocity = 0;
            const short rs = GTN_GetSts(core, axis, &status);
            const short rv = GTN_GetAxisEncVel(core, axis, &velocity);
            stopped = rs == 0 && rv == 0 && !(status & movingMask) && std::abs(velocity) <= velocityTol;
            if (!stopped) std::this_thread::sleep_for(10ms);
        } while (!stopped && Clock::now() < until);
        try {
            record << "smooth_stop_rc=" << rc << "\nstop_feedback="
                   << (stopped ? "stationary" : "unconfirmed") << '\n' << std::flush;
            if (rc || !stopped) std::cerr << "Smooth stop unconfirmed. Check axis on site.\n";
        } catch (...) {}
    }

    static long long hostUs(Clock::time_point time) {
        return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
    }

private:
    Recorder& card;
    std::ostream& log;
    Monitor monitor;
    double followLimit = 0;
    bool active = false;
};

inline Tick event(Recorder& card, std::ostream& log, const std::string& label) {
    unsigned long tick = 0;
    card.check(label + ".GTN_GetClockHighPrecision", GTN_GetClockHighPrecision(core, &tick));
    log << label << "_tick=" << tick << '\n' << label << "_host_us="
        << Axis::hostUs(Clock::now()) << '\n' << std::flush;
    return tick;
}

inline void confirmTail(Axis& motor, Recorder& card, std::ostream& log, std::ostream& trace, long target) {
    const auto deadline = Clock::now() + 500ms;
    TailWindow window;
    Tick firstTick = 0;
    while (Clock::now() < deadline) {
        require(!stopRequested(), "operator_stop");
        unsigned long beforeSample = 0;
        const short clockRc = GTN_GetClockHighPrecision(core, &beforeSample);
        if (clockRc != 0) card.check("tail.GTN_GetClockHighPrecision", clockRc);
        const auto s = motor.read(&trace, "tail");
        require(s.atTailPosition(target), "tail_motion_flag_or_endpoint_outside_tolerance");
        const auto now = Axis::hostUs(Clock::now());
        const bool complete = window.extend(now, s.x);
        if (window.startUs == now) firstTick = beforeSample;
        if (complete) {
            log << "tail_stable_begin_tick=" << firstTick << "\ntail_span_limit_pulse="
                << tailSpanPulse << "\ntail_host_observed_span_pulse="
                << window.maximum - window.minimum << '\n';
            event(card, log, "tail_stable_end");
            return;
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("tail_encoder_did_not_stabilize");
}

inline bool saveWatch(Recorder& card, std::ostream& log, std::ostream& batches, std::ostream& raw,
               Clock::time_point started, unsigned long capacity, Tick first, Tick last) {
    bool stoppedFirstTime = true;
    try {
        card.stop();
    } catch (const std::exception&) {
        stoppedFirstTime = false;
        log << "watch_stop_retry=1\n" << std::flush;
        card.stop();
    }
    const auto frozen = card.info("stopped");
    const auto data = readColumns(log, batches, started, capacity);
    const auto& columns = data.columns;
    const auto count = std::max({columns[0].size(), columns[1].size(), columns[2].size()});
    bool contiguous = true;
    Tick previous = 0;
    for (std::size_t i = 0; i < count; ++i) {
        raw << i;
        for (const auto& column : columns) {
            raw << ',';
            if (i < column.size()) raw << column[i];
        }
        raw << '\n';
        if (i >= columns[0].size()) continue;
        const double tick = columns[0][i];
        if (!std::isfinite(tick) || tick != std::trunc(tick) || tick < INT32_MIN || tick > UINT32_MAX) {
            contiguous = false;
            continue;
        }
        const auto current = static_cast<Tick>(static_cast<std::int64_t>(tick));
        contiguous = contiguous && (!i || current - previous == 1) &&
                     current - first <= last - first;
        previous = current;
    }
    raw.flush();
    const auto after = card.info("after_read");
    return stoppedFirstTime && data.aligned && contiguous && frozen.run == 0 && after.run == 0 &&
           frozen.time == after.time && frozen.head == after.head && count == frozen.time &&
           after.varCount == channels.size();
}

struct Preparation {
    std::vector<long> waypoints;  // Ordered absolute pulse positions before the run start.
    double speed = transferSpeed, holdMs = 0;
    std::string metadata;
};

inline bool recordRun(Recorder& card, Axis& motor, const fs::path& root, const std::string& id,
               int pair, int order, const Plan& plan, const Run& run, Ns tickNs,
               double reserve, double stopDeceleration, const Preparation& prep = {}) {
    const auto key = id + "_p" + std::to_string(pair) + "_r" + std::to_string(order);
    auto log = createFile(root / "manifest" / (key + ".txt"));
    auto trace = createFile(root / "data/raw" / (key + ".sdk.csv"));
    auto raw = createFile(root / "data/raw" / (key + ".csv"));
    auto batches = createFile(root / "data/raw" / (key + ".batches.csv"));
    log << "session_id=" << id << "\npair_id=" << pair << "\nrun_id=" << key
        << "\nexecution_order=" << order << "\nrole=" << run.role
        << "\ndirection=" << plan.recipe.direction << "\ncore_period_ns=" << tickNs
        << "\nx0_plan_mm=" << plan.recipe.planCenter << "\nx_match_eval_mm=" << plan.recipe.evalCenter
        << "\nstart_mm=" << mm(run.start) << "\nend_mm=" << mm(run.end)
        << "\nsdk_clock_unit=card_ms_for_individual_fields\nhost_timebase=steady_clock_us"
        << "\ncard_event_timebase=interrupt_tick\n" << prep.metadata << std::flush;
    trace << "phase,host_begin_us,host_end_us,encoder_pulse,velocity_pulse_ms,status,"
             "planner_pulse,planner_pos_card_ms,planner_vel_pulse_ms,planner_vel_card_ms,"
             "following_error_pulse,following_error_card_ms,rc_x,rc_v,rc_sts,rc_mode,"
             "rc_planner,rc_limits,rc_plan_x,rc_plan_v,rc_error\n";
    raw << csvHeader << '\n';
    batches << "column,channel_code,host_read_begin_us,host_read_end_us,frames,rc,overflow\n";
    bool watchStarted = false, finished = false;
    const auto watchLimit = static_cast<unsigned long>(std::ceil(5.5e9 / double(tickNs)));
    require(watchLimit <= maxReadCapacity, "Watch read capacity exceeded");
    Clock::time_point watchOrigin{};
    Tick watchStart = 0;
    std::string reason = "none";
    try {
        auto points = prep.waypoints;
        points.push_back(run.start);
        if (!prep.metadata.empty()) {
            log << "preparation_speed_mm_s=" << prep.speed << "\npreparation_hold_ms=" << prep.holdMs
                << "\npreparation_targets_pulse=";
            for (std::size_t i = 0; i < points.size(); ++i) log << (i ? "," : "") << points[i];
            log << '\n';
        }
        for (long point : points) {
            const auto initial = motor.read(&trace, "transfer_before");
            require(initial.idle() && std::abs(mm(initial.x - initial.planX)) <= positionTol,
                    "not_stationary_or_feedback_planner_mismatch");
            require(pathCheck(log, "Actual transfer", mm(initial.x), mm(point), prep.speed,
                              reserve, stopDeceleration), "insufficient_actual_transfer_reserve");
            if (!initial.at(point)) {
                motor.setTrap(point, prep.speed, transferAcc, log, "transfer", tickNs);
                motor.issue();
                motor.settle(pulse(mm(initial.x)), point, std::max(20.0, upper / prep.speed + 4), &trace, "transfer");
            }
        }
        const auto prepared = Clock::now() + std::chrono::duration<double, std::milli>(prep.holdMs);
        while (Clock::now() < prepared) {
            motor.readyAt(run.start, &trace, "preparation_hold");
            std::this_thread::sleep_for(5ms);
        }
        motor.readyAt(run.start, &trace, "before");
        const auto [trap, speed] = motor.setTrap(run.end, plan.recipe.peak, plan.recipe.acc, log, "run", tickNs);
        auto readback = plan.recipe;
        readback.peak = speed;
        const auto actual = timingFor(readback, run.role, run.start, run.end,
                                      trap.acc * 1e6 / pulsePerMm, trap.dec * 1e6 / pulsePerMm);
        log << "planned_platform_begin_ms=" << actual.platformBegin * 1000
            << "\nplanned_platform_end_ms=" << actual.platformEnd * 1000
            << "\nplanned_match_ms=" << actual.matchSec * 1000
            << "\nplanned_eval_crossing_ms=" << actual.evalSec * 1000
            << "\nplanned_duration_ms=" << actual.duration * 1000
            << "\nplanned_match_speed_mm_s=" << actual.matchSpeed << '\n';
        card.configure();
        watchStart = event(card, log, "watch_before");
        watchOrigin = Clock::now();
        card.start();
        watchStarted = true;
        std::this_thread::sleep_for(10ms);
        const auto state = card.info("started");
        require(state.run && state.mode == WATCH_MODE_STATIC && state.interval == interval &&
                state.varCount == channels.size(), "Watch_did_not_start");
        const auto hold = [&](long target, double duration, const char* phase) {
            const auto until = Clock::now() + std::chrono::duration<double, std::milli>(duration);
            do {
                motor.readyAt(target, &trace, phase);
                std::this_thread::sleep_for(5ms);
            } while (Clock::now() < until);
        };
        hold(run.start, baselineMs, "baseline");
        event(card, log, "command_before");
        motor.issue();
        event(card, log, "command_after");
        motor.settle(run.start, run.end, 4, &trace, "motion");
        event(card, log, "settled");
        confirmTail(motor, card, log, trace, run.end);
        finished = true;
    } catch (const std::exception& e) {
        reason = e.what();
        motor.abort(log);
    }
    bool aligned = false;
    if (watchStarted) {
        try {
            // The host read begins only after Watch is frozen. The second
            // bracket allows for SDK stop latency while retaining raw data.
            unsigned long stopBound = 0;
            const short beforeStop = GTN_GetClockHighPrecision(core, &stopBound);
            log << "watch_end_request_rc=" << beforeStop << "\nwatch_end_request_tick=" << stopBound << '\n';
            aligned = saveWatch(card, log, batches, raw, watchOrigin, watchLimit, watchStart,
                                stopBound + static_cast<Tick>(std::ceil(0.1e9 / double(tickNs))));
            event(card, log, "watch_read_complete");
            aligned = aligned && beforeStop == 0;
        } catch (const std::exception& e) {
            if (reason == "none") reason = e.what();
        }
    }
    if (!aligned && reason == "none") reason = "Watch_alignment_or_continuity_failed";
    log << "watch_aligned_contiguous=" << aligned << "\noutcome="
        << (finished && aligned ? "complete" : "incomplete") << "\nreason=" << reason << '\n' << std::flush;
    std::cout << key << (finished && aligned ? " complete" : " incomplete: " + reason) << '\n';
    return finished && aligned;
}

inline std::vector<double> numbers(const std::string& input, std::size_t count) {
    std::vector<double> values;
    std::istringstream stream(input);
    std::string value;
    while (std::getline(stream, value, ',')) values.push_back(parseNumber(value));
    require(values.size() == count && input.back() != ',', "Wrong number of comma-separated values");
    return values;
}

}  // namespace matched_motion
