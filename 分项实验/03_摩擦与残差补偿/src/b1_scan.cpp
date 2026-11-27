#include "matched_batch.hpp"

#include <deque>
#include <io.h>

namespace {
using namespace matched_batch;

constexpr double sampleHoldMs = 1000;
constexpr double maxStableWaitMs = 2000;
constexpr double transferLimitMmS = 5;
constexpr Ns expectedTickNs = 50000;

struct Limits {
    double lo, hi, reserve, follow, minStop, maxSpeed;
};

struct Slot {
    std::string id;
    long lo, hi;
    int speed, acc, first;
};

bool validCondition(const Slot& s)
{
    if (!((s.lo == 10000 && s.hi == 22000) || (s.lo == 12000 && s.hi == 20000))) return false;
    if (s.speed == 2 || s.speed == 5) return s.acc == 400;
    if (s.speed == 10) return s.acc == 400 || s.acc == 600;
    return (s.speed == 15 || s.speed == 20 || s.speed == 30 || s.speed == 40) &&
           (s.acc == 400 || s.acc == 500 || s.acc == 600);
}

fs::path rootDirectory()
{
    wchar_t path[32768]{};
    const auto size = GetModuleFileNameW(nullptr, path, 32768);
    require(size && size < 32768, "Cannot locate executable");
    for (auto dir = fs::path(path).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::is_regular_file(dir / "ZRFrictionCurrent.vcxproj")) return dir;
        if (dir == dir.root_path()) break;
    }
    throw std::runtime_error("Cannot locate third-stage project");
}

void clearance(const Limits& l, double start, long end, double stop, double maxSpeed)
{
    const double speed = maxSpeed * pulsePerMm;
    const double need = l.reserve + l.follow + positionTol * pulsePerMm + speed * speed / (2 * stop * 1e6);
    require(std::min(start, double(end)) - l.lo >= need && l.hi - std::max(start, double(end)) >= need,
            "B1 path or transfer fails stopping clearance");
}

class Monitor {
public:
    Monitor(const Limits& limits, std::ostream& trace) : settings(limits), log(trace) {}

    void reset(double speed) { maximumSpeed = speed; positionHistory.clear(); }

    void check(const State& s)
    {
        const auto now = Clock::now();
        short current = 0;
        // Watch provides the required current channel; these host reads supplement the log.
        const short currentRc = GTN_GetAtlTorque(core, axis, &current);
        if (temperatureTime == Clock::time_point{} || now - temperatureTime >= 250ms) {
            temperatureRc = GTN_GetTemperature(core, &temperature);
            temperatureTime = now;
        }
        positionHistory.emplace_back(now, s.x);
        while (positionHistory.size() > 2 && now - positionHistory[1].first >= 20ms)
            positionHistory.pop_front();
        double measuredSpeed = 0;
        if (now - positionHistory.front().first >= 20ms) {
            const double span = std::chrono::duration<double>(now - positionHistory.front().first).count();
            measuredSpeed = std::abs(mm(s.x - positionHistory.front().second)) / span;
        }
        log << Axis::hostUs(now) << ',';
        if (!currentRc) log << current;
        log << ',';
        if (!temperatureRc && std::isfinite(temperature)) log << temperature;
        log << ',' << measuredSpeed << ',' << mm(s.planV * 1000) << ',' << s.x << ',' << s.error
            << ',' << s.status << ',' << currentRc << ',' << temperatureRc << '\n';
        require(s.x >= settings.lo && s.x <= settings.hi, "B1 feedback outside approved protection range");
        require(measuredSpeed <= maximumSpeed && std::abs(s.planV * 1000 / pulsePerMm) <= maximumSpeed,
                "B1 velocity protection exceeded");
    }

private:
    const Limits& settings;
    std::ostream& log;
    Clock::time_point temperatureTime{};
    std::deque<std::pair<Clock::time_point, double>> positionHistory;
    double temperature = 0;
    short temperatureRc = -1;
    double maximumSpeed = transferLimitMmS;
};

void hold(Axis& motor, long target, std::ostream& sdk, const char* phase)
{
    const auto deadline = Clock::now() + std::chrono::duration<double, std::milli>(maxStableWaitMs);
    Clock::time_point start{}, previous{};
    double low = 0, high = 0;
    while (Clock::now() < deadline) {
        require(!stopRequested(), "operator_stop");
        const auto s = motor.read(&sdk, phase);
        const auto now = Clock::now();
        const bool settled = s.atTailPosition(target) && std::abs(s.planV) <= velocityTol;
        if (!settled) {
            start = {};
        } else {
            if (start == Clock::time_point{} || now - previous > 50ms ||
                std::max(high, s.x) - std::min(low, s.x) > positionTol * pulsePerMm) {
                start = now;
                low = high = s.x;
            }
            low = std::min(low, s.x);
            high = std::max(high, s.x);
            if (now - start >= std::chrono::duration<double, std::milli>(sampleHoldMs)) return;
        }
        previous = now;
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("B1 did not establish continuous settled hold");
}

void prepare(Axis& motor, long start, const Limits& limits, double dec, Ns tick,
             Monitor& monitor, std::ostream& log, std::ostream& sdk)
{
    monitor.reset(transferLimitMmS);
    const auto initial = motor.read(&sdk, "transfer_before");
    require(initial.idle() && std::abs(initial.x - initial.planX) <= positionTol * pulsePerMm &&
            std::abs(initial.planV) <= velocityTol, "B1 transfer initial state is not idle/aligned");
    if (initial.at(start)) return;
    clearance(limits, initial.x, start, dec, transferLimitMmS);
    motor.setTrap(start, transferSpeed, transferAcc, log, "transfer", tick);
    motor.issue();
    motor.settle(pulse(mm(initial.x)), start, 25, &sdk, "transfer");
}

bool recordLeg(Recorder& card, Axis& motor, Monitor& monitor, const Limits& limits, Ns tick, double dec,
               const fs::path& rawDir, const Slot& slot, int leg)
{
    const int direction = leg == 1 ? slot.first : -slot.first;
    const long start = direction > 0 ? slot.lo : slot.hi;
    const long end = direction > 0 ? slot.hi : slot.lo;
    const auto key = slot.id + "_leg" + std::to_string(leg);
    auto log = createFile(rawDir / (key + ".txt"));
    auto raw = createFile(rawDir / (key + ".csv"));
    auto sdk = createFile(rawDir / (key + ".sdk.csv"));
    auto batches = createFile(rawDir / (key + ".batches.csv"));
    raw << csvHeader << '\n';
    sdk << "phase,host_begin_us,host_end_us,encoder_pulse,velocity_pulse_ms,status,planner_pulse,"
           "planner_pos_card_ms,planner_vel_pulse_ms,planner_vel_card_ms,following_error_pulse,following_error_card_ms,"
           "rc_x,rc_v,rc_sts,rc_mode,rc_planner,rc_limits,rc_plan_x,rc_plan_v,rc_error\n";
    batches << "column,type,host_begin_us,host_end_us,rc,capacity,count\n";
    log << "schema=b1_watch_v1\nslot_id=" << slot.id << "\nleg=" << leg << "\ndirection=" << direction
        << "\ncore_period_ns=" << tick << "\nwatch_interval_argument=" << interval << "\nwatch_expected_frame_ns=" << tick
        << "\ntrap_interpolation_period_separately_configured=0\nhost_poll_request_ms=5\nanalysis_grid_ms=5"
        << "\nstart_pulse=" << start << "\nend_pulse=" << end << "\nnominal_speed_mm_s=" << slot.speed
        << "\nnominal_acceleration_mm_s2=" << slot.acc << "\nsettled_hold_ms=" << sampleHoldMs
        << "\nsettled_wait_limit_ms=" << maxStableWaitMs << '\n';

    bool started = false, complete = false;
    std::string reason = "none";
    auto origin = Clock::now();
    Tick watchStart = 0;
    // Bound the read buffer independently of the card's actual Watch storage capacity.
    const double duration = mm(std::abs(end - start)) / slot.speed + double(slot.speed) / slot.acc;
    const double motionTimeout = duration + 1.0;
    const auto capacity = static_cast<unsigned long>(std::ceil((motionTimeout + 4.5) * 1e9 / tick));
    require(capacity <= maxReadCapacity, "B1 Watch read capacity exceeded");

    try {
        prepare(motor, start, limits, dec, tick, monitor, log, sdk);
        monitor.reset(limits.maxSpeed);
        clearance(limits, start, end, dec, limits.maxSpeed);
        const auto [actual, speed] = motor.setTrap(end, slot.speed, slot.acc, log, "measurement", tick);
        const auto ramps = std::pow(speed, 2) / (2 * (actual.acc * 1e6 / pulsePerMm)) +
                           std::pow(speed, 2) / (2 * (actual.dec * 1e6 / pulsePerMm));
        require(ramps < mm(std::abs(end - start)), "B1 TRAP readback cannot reach scheduled speed");
        card.configure();
        watchStart = event(card, log, "watch_before");
        origin = Clock::now();
        card.start();
        started = true;
        std::this_thread::sleep_for(10ms);
        const auto watch = card.info("started");
        require(watch.run && watch.mode == WATCH_MODE_STATIC && watch.interval == interval &&
                watch.varCount == channels.size(), "B1 Watch did not start");
        hold(motor, start, sdk, "pre_hold");
        event(card, log, "command_before");
        motor.issue();
        event(card, log, "command_after");
        motor.settle(start, end, motionTimeout, &sdk, "motion");
        event(card, log, "settled");
        hold(motor, end, sdk, "post_hold");
        event(card, log, "post_hold_complete");
        complete = true;
    } catch (const std::exception& e) {
        reason = e.what();
        motor.abort(log);
    }

    bool aligned = false;
    if (started) {
        try {
            const Tick endTick = event(card, log, "watch_end_request");
            aligned = saveWatch(card, log, batches, raw, origin, capacity, watchStart,
                                endTick + static_cast<Tick>(std::ceil(.1e9 / tick)));
        } catch (const std::exception& e) {
            if (reason == "none") reason = e.what();
        }
    }
    if (!aligned && reason == "none") reason = "B1 Watch alignment or continuity failed";
    log << "watch_aligned_contiguous=" << aligned << "\noutcome="
        << (complete && aligned ? "complete" : "incomplete") << "\nreason=" << reason << std::endl;
    std::cout << key << ' ' << (complete && aligned ? "complete" : reason) << std::endl;
    return complete && aligned;
}

int runBatch(const fs::path& root, const std::string& protocol, const std::string& attempt)
{
    require(safeId(protocol) && safeId(attempt), "Invalid B1 protocol or attempt ID");
    const auto dir = root / "B1" / "manifest" / protocol / "attempts" / attempt;
    std::ifstream request(dir / "request.txt");
    if (!fs::is_regular_file(dir / "pre.txt") || fs::exists(dir / "native.txt"))
        throw std::runtime_error("Missing independent B1 precheck or the attempt has already started");
    require(readFields(dir / "pre.txt").at("ready") == "1", "B1 precheck did not pass");
    Limits l{};
    int count = 0;
    require(bool(request >> l.lo >> l.hi >> l.reserve >> l.follow >> l.minStop >>
                 l.maxSpeed >> count), "Invalid B1 batch request");
    require(l.lo >= 0 && l.lo < 10000 && l.hi <= 37000 && l.hi > 22000 &&
            l.reserve >= 1 && l.follow >= 1 && l.follow <= 500 && l.minStop > 0 &&
            l.maxSpeed > 40 && l.maxSpeed <= 60 && count >= 1 && count <= 4,
            "B1 batch parameters outside protocol bounds");
    std::vector<Slot> slots;
    for (int i = 0; i < count; ++i) {
        Slot s{};
        require(bool(request >> s.id >> s.lo >> s.hi >> s.speed >> s.acc >> s.first) && safeId(s.id) &&
                validCondition(s) && std::abs(s.first) == 1, "Invalid B1 slot");
        require(std::none_of(slots.begin(), slots.end(), [&](const auto& p) { return p.id == s.id; }),
                "Duplicate B1 slot");
        slots.push_back(s);
        std::cout << s.id << " | " << (s.first == 1 ? s.lo : s.hi) << " -> "
                  << (s.first == 1 ? s.hi : s.lo) << " -> " << (s.first == 1 ? s.lo : s.hi)
                  << " pulse | " << s.speed << " mm/s | " << s.acc << " mm/s^2\n";
    }
    std::string extra;
    require(!(request >> extra), "Unexpected B1 request data");
    require(line("Type START to execute the displayed B1 batch: ") == "START", "B1 batch cancelled");
    require(SetConsoleCtrlHandler(onCtrl, TRUE) != FALSE, "Cannot install B1 stop handler");
    const auto rawDir = root / "B1" / "data" / "raw" / protocol / attempt;
    fs::create_directories(rawDir);
    auto log = createFile(dir / "native.txt");
    auto trace = createFile(rawDir / "guards.csv");
    trace << "host_us,current_raw,card_temp_c,estimated_abs_speed_mm_s,planner_mm_s,encoder_pulse,following_error_pulse,status,rc_current,rc_card_temp\n";
    Recorder card(log);
    Monitor guard(l, trace);
    Axis motor(card, log, [&](const State& s) { guard.check(s); });
    bool complete = false;
    try {
        card.connect();
        const auto tick = verifyCorePeriod(card, log);
        require(tick == expectedTickNs, "B1 frozen Watch timebase requires a verified 50 us core");
        motor.setFollowLimit(l.follow);
        double smooth = 0, abrupt = 0;
        card.check("GTN_GetStopDec", GTN_GetStopDec(core, axis, &smooth, &abrupt));
        log << "smooth_stop_dec_pulse_ms2=" << smooth << "\nabrupt_stop_dec_pulse_ms2=" << abrupt << std::endl;
        require(std::isfinite(smooth) && smooth >= l.minStop && std::isfinite(abrupt) && abrupt > 0,
                "B1 stop protection readback failed");
        auto state = motor.read();
        require(state.idle() && std::abs(state.x - state.planX) <= positionTol * pulsePerMm &&
                std::abs(state.planV) <= velocityTol, "B1 initial position is not idle/aligned");
        double position = state.x;
        for (const auto& s : slots) {
            const long start = s.first > 0 ? s.lo : s.hi;
            if (std::abs(position - start) > 10)
                clearance(l, position, start, smooth, transferLimitMmS);
            clearance(l, s.lo, s.hi, smooth, l.maxSpeed);
            position = start;
        }
        complete = true;
        for (const auto& s : slots) {
            if (!recordLeg(card, motor, guard, l, tick, smooth, rawDir, s, 1) ||
                !recordLeg(card, motor, guard, l, tick, smooth, rawDir, s, 2)) {
                complete = false;
                break;
            }
        }
    } catch (const std::exception& e) {
        motor.abort(log);
        log << "reason=" << e.what() << std::endl;
        std::cerr << e.what() << '\n';
    }
    card.cleanup();
    complete = complete && card.cleanupOk;
    log << "outcome=" << (complete ? "complete" : "incomplete") << std::endl;
    return complete ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string(argv[1]) == "--state") return readStatus();
        if (argc == 4 && std::string(argv[1]) == "--run-batch") return runBatch(rootDirectory(), argv[2], argv[3]);
        std::cout << "Stage 3 B1: use scripts/b1_run.py --plan, --freeze, --batch or --replay.\n"
                     "This native executable is called by the B1 console script.\n";
        if (argc == 1 && _isatty(_fileno(stdin))) {
            std::cout << "Press Enter to close." << std::endl;
            std::cin.get();
        }
        return argc == 1 ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "B1: " << e.what() << '\n';
        return 1;
    }
}
