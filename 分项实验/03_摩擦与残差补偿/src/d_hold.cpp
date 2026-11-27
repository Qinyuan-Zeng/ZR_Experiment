#include "matched_batch.hpp"

#include <deque>

namespace {
using namespace matched_batch;

constexpr long stroke = 4000;
constexpr int holdMs = 6000;
constexpr double acceleration = 400, approachSpeed = 10;
constexpr double transferCap = 5, measurementCap = 15;
constexpr Ns tickNs = 50000;

struct Limits { double lo, hi, reserve, follow, stop; };
struct Trial {
    std::string id;
    long target;
    int sign;
    long start() const { return target - sign * stroke; }
};

fs::path rootDirectory()
{
    wchar_t module[32768]{};
    const auto n = GetModuleFileNameW(nullptr, module, 32768);
    require(n > 0 && n < 32768, "cannot_locate_D_executable");
    for (auto p = fs::path(module).parent_path(); p != p.root_path(); p = p.parent_path())
        if (fs::is_regular_file(p / "ZRFrictionCurrent.vcxproj")) return p;
    throw std::runtime_error("cannot_locate_stage3_project");
}

void clearance(const Limits& l, double start, long end, double cap)
{
    const double need = l.reserve + l.follow + 10 + cap * cap / (2 * l.stop);
    require(std::min(start, double(end)) - l.lo >= need &&
            l.hi - std::max(start, double(end)) >= need, "D_stopping_clearance_failed");
}

class Guard {
public:
    explicit Guard(const Limits& limits, std::ostream& stream) : l(limits), log(stream) {}
    double cap = transferCap;

    void check(const State& s)
    {
        require(s.x >= l.lo && s.x <= l.hi, "D_feedback_outside_boundary");
        if (!positions.empty()) {
            const auto dt = s.encoderClock - positions.back().first;
            require(dt < 0x80000000UL, "D_encoder_clock_reversed");
            // Submillisecond observations can differ in position at an equal SDK timestamp.
            if (dt == 0) {
                require(std::abs(s.x - firstInTick) <= cap + 2, "D_encoder_jump_within_one_ms");
                positions.back().second = s.x;
            } else {
                positions.emplace_back(s.encoderClock, s.x);
                firstInTick = s.x;
            }
        } else {
            positions.emplace_back(s.encoderClock, s.x);
            firstInTick = s.x;
        }
        while (positions.size() > 2 && s.encoderClock - positions[1].first >= 20)
            positions.pop_front();
        const auto dt = s.encoderClock - positions.front().first;
        const double speed = dt >= 20 ? std::abs(s.x - positions.front().second) / dt : 0;
        log << Axis::hostUs(Clock::now()) << ',' << s.encoderClock << ',' << s.x << ','
            << s.planV << ',' << speed << ',' << s.error << ',' << s.status << '\n';
        require(std::abs(s.planV) <= cap && speed <= cap, "D_velocity_protection_exceeded");
    }

private:
    const Limits& l;
    std::ostream& log;
    std::deque<std::pair<unsigned long, double>> positions;
    double firstInTick = 0;
};

void move(Axis& motor, Recorder& card, std::ostream& log, std::ostream& sdk,
          long from, long to, double speed, double acc, const char* role)
{
    const auto [trap, actualSpeed] = motor.setTrap(to, speed, acc, log, role, tickNs);
    const double ramp = actualSpeed * actualSpeed / (2000 * trap.acc) +
                        actualSpeed * actualSpeed / (2000 * trap.dec);
    if (std::string(role) != "transfer")
        require(ramp < mm(std::abs(to - from)), "D_no_velocity_plateau");
    const std::string label(role);
    event(card, log, label + "_before");
    motor.issue();
    event(card, log, label + "_after");
    motor.settle(from, to, mm(std::abs(to - from)) / speed + 3, &sdk, role);
    event(card, log, label + "_settled");
}

void hold(Axis& motor, Recorder& card, std::ostream& log, std::ostream& sdk,
          long target, int durationMs, const char* role)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(durationMs + 600);
    Clock::time_point began{}, previous{};
    double lo = 0, hi = 0;
    while (Clock::now() < deadline) {
        require(!stopRequested(), "operator_stop");
        const auto s = motor.read(&sdk, role);
        const auto now = Clock::now();
        if (!s.atTailPosition(target) || std::abs(s.planV) > velocityTol) {
            began = {};
        } else {
            if (began == Clock::time_point{} || now - previous > 50ms ||
                std::max(hi, s.x) - std::min(lo, s.x) > 10) {
                began = now;
                lo = hi = s.x;
                event(card, log, std::string(role) + "_begin");
                began = Clock::now();
            }
            lo = std::min(lo, s.x);
            hi = std::max(hi, s.x);
            if (now - began >= std::chrono::milliseconds(durationMs)) {
                event(card, log, std::string(role) + "_end");
                return;
            }
        }
        previous = now;
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("D_continuous_hold_not_established");
}

bool capture(Recorder& card, Axis& motor, Guard& guard, const Limits& l,
             const fs::path& rawDir, const Trial& t)
{
    auto log = createFile(rawDir / (t.id + ".txt"));
    auto raw = createFile(rawDir / (t.id + ".csv"));
    auto sdk = createFile(rawDir / (t.id + ".sdk.csv"));
    auto batches = createFile(rawDir / (t.id + ".batches.csv"));
    raw << csvHeader << '\n';
    sdk << "phase,host_begin_us,host_end_us,encoder_pulse,velocity_pulse_ms,status,planner_pulse,"
           "planner_pos_card_ms,planner_vel_pulse_ms,planner_vel_card_ms,following_error_pulse,following_error_card_ms,"
           "rc_x,rc_v,rc_sts,rc_mode,rc_planner,rc_limits,rc_plan_x,rc_plan_v,rc_error\n";
    batches << "column,type,host_begin_us,host_end_us,rc,capacity,count\n";
    log << "schema=d_hold_v1\nslot_id=" << t.id << "\ndirection=" << t.sign
        << "\nstart_pulse=" << t.start() << "\ntarget_pulse=" << t.target
        << "\nhold_ms=" << holdMs << "\ncore_period_ns=" << tickNs << '\n';
    bool started = false, complete = false;
    auto origin = Clock::now();
    Tick watchStart = 0;
    std::string reason = "none";
    try {
        guard.cap = transferCap;
        const auto initial = motor.read(&sdk, "transfer");
        require(initial.idle() && std::abs(initial.x - initial.planX) <= 10,
                "D_transfer_initial_state_not_idle");
        clearance(l, initial.x, t.start(), transferCap);
        if (!initial.at(t.start()))
            move(motor, card, log, sdk, static_cast<long>(std::lround(initial.x)),
                 t.start(), transferSpeed, transferAcc, "transfer");

        card.configure();
        watchStart = event(card, log, "watch_before");
        origin = Clock::now();
        card.start();
        started = true;
        std::this_thread::sleep_for(10ms);
        const auto watch = card.info("started");
        require(watch.run && watch.mode == WATCH_MODE_STATIC && watch.interval == interval &&
                watch.varCount == channels.size(), "D_Watch_did_not_start");
        hold(motor, card, log, sdk, t.start(), 250, "setup_hold");
        guard.cap = measurementCap;
        move(motor, card, log, sdk, t.start(), t.target, approachSpeed, acceleration, "approach");
        hold(motor, card, log, sdk, t.target, holdMs, "hold");
        complete = true;
    } catch (const std::exception& e) {
        reason = e.what();
        motor.abort(log);
    }

    bool aligned = false;
    if (started) {
        try {
            const auto last = event(card, log, "watch_end_request");
            aligned = saveWatch(card, log, batches, raw, origin, 300000, watchStart, last + 2000);
        } catch (const std::exception& e) {
            if (reason == "none") reason = e.what();
        }
    }
    if (!aligned && reason == "none") reason = "D_Watch_continuity_failed";
    log << "watch_aligned_contiguous=" << aligned << "\noutcome="
        << (complete && aligned ? "complete" : "incomplete") << "\nreason=" << reason << '\n';
    std::cout << t.id << ' ' << reason << std::endl;
    return complete && aligned;
}

int runBatch(const std::string& protocol, const std::string& attempt)
{
    require(safeId(protocol) && safeId(attempt), "invalid_D_identifier");
    const auto root = rootDirectory();
    const auto dir = root / "D" / "manifest" / protocol / "attempts" / attempt;
    require(!fs::exists(dir / "native.txt") && readFields(dir / "pre.txt").at("ready") == "1",
            "D_missing_precheck_or_existing_attempt");
    std::ifstream in(dir / "request.txt");
    Limits l{};
    int count = 0;
    require(bool(in >> l.lo >> l.hi >> l.reserve >> l.follow >> l.stop >> count) &&
            l.lo >= 0 && l.lo < 8000 && l.hi > 24000 && l.hi <= 37000 &&
            l.reserve >= 1 && l.follow >= 1 && l.follow <= 500 && l.stop > 0 &&
            count >= 1 && count <= 3, "invalid_D_limits_or_batch_size");
    std::vector<Trial> trials;
    for (int i = 0; i < count; ++i) {
        Trial t{};
        require(bool(in >> t.id >> t.target >> t.sign) && safeId(t.id) &&
                (t.sign == 1 || t.sign == -1) &&
                (t.target == 12000 || t.target == 16000 || t.target == 20000) &&
                std::none_of(trials.begin(), trials.end(), [&](const auto& item) { return item.id == t.id; }),
                "invalid_D_trial");
        trials.push_back(t);
    }
    std::string extra;
    require(!(in >> extra), "unexpected_D_request_data");
    require(line("Type START to execute the displayed D batch: ") == "START", "D_batch_cancelled");
    require(SetConsoleCtrlHandler(onCtrl, TRUE) != FALSE, "cannot_install_D_stop_handler");

    const auto rawDir = root / "D" / "data" / "raw" / protocol / attempt;
    fs::create_directories(rawDir);
    auto log = createFile(dir / "native.txt");
    auto guardLog = createFile(rawDir / "guard.csv");
    guardLog << "host_us,encoder_card_ms,position_pulse,planner_mm_s,estimated_abs_mm_s,following_error_pulse,status\n";
    Recorder card(log);
    Guard guard(l, guardLog);
    Axis motor(card, log, [&](const State& s) { guard.check(s); });
    bool complete = false;
    try {
        card.connect();
        require(verifyCorePeriod(card, log) == tickNs, "D_requires_50_us_core");
        motor.setFollowLimit(l.follow);
        double smooth = 0, abrupt = 0;
        card.check("GTN_GetStopDec", GTN_GetStopDec(core, axis, &smooth, &abrupt));
        log << "smooth_stop_dec_pulse_ms2=" << smooth << "\nabrupt_stop_dec_pulse_ms2=" << abrupt << '\n';
        require(std::isfinite(smooth) && smooth >= l.stop && std::isfinite(abrupt) && abrupt > 0,
                "D_stop_readback_failed");
        const auto initial = motor.read();
        unsigned long clock = 0;
        card.check("GTN_GetClock", GTN_GetClock(core, &clock));
        require(clock - initial.encoderClock <= 50 && initial.idle() &&
                std::abs(initial.x - initial.planX) <= 10, "D_initial_readback_failed");
        double pos = initial.x;
        for (const auto& t : trials) {
            clearance(l, pos, t.start(), transferCap);
            clearance(l, t.start(), t.target, measurementCap);
            pos = t.target;
        }
        complete = true;
        for (const auto& t : trials) {
            if (!capture(card, motor, guard, l, rawDir, t)) {
                complete = false;
                break;
            }
        }
    } catch (const std::exception& e) {
        motor.abort(log);
        log << "reason=" << e.what() << '\n';
        std::cerr << e.what() << '\n';
    }
    card.cleanup();
    complete = complete && card.cleanupOk;
    log << "outcome=" << (complete ? "complete" : "incomplete") << '\n';
    return complete ? 0 : 1;
}
}

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string(argv[1]) == "--state") return readStatus();
        if (argc == 4 && std::string(argv[1]) == "--run-batch") return runBatch(argv[2], argv[3]);
        std::cout << "Stage 3 D: scripts/d_run.py --plan, --freeze, --pending, --batch or --replay.\n";
        return argc == 1 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "D: " << e.what() << '\n';
        return 1;
    }
}
