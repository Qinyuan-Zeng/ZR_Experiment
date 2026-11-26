#define NOMINMAX
#include <windows.h>
#include <conio.h>
#include "gts.h"
#include "stage1.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <atomic>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Time = Clock::time_point;
constexpr short core = 2, axis = 1;
constexpr long axisMask = 1L << (axis - 1);
constexpr long alarmMask = 1L << 1;
constexpr long faultMask = alarmMask | (1L << 5) | (1L << 6);
constexpr long enableMask = 1L << 9, movingMask = 1L << 10;
constexpr double pulsePerMm = 1000, upper = 32;
constexpr double upperPulse = upper * pulsePerMm;
constexpr double endpointAllowancePulse = 10;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();
constexpr char version[] = "stage1-14";
constexpr double scanDerivativeSpan = .05;
std::atomic<bool> interrupted{false};

BOOL WINAPI handleInterrupt(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    interrupted.store(true);
    return TRUE;
}

class InterruptHandler {
public:
    InterruptHandler()
    {
        if (!SetConsoleCtrlHandler(handleInterrupt, TRUE))
            throw std::runtime_error("cannot_register_console_stop");
    }
    ~InterruptHandler() { SetConsoleCtrlHandler(handleInterrupt, FALSE); }
};

double seconds(Time since)
{
    return std::chrono::duration<double>(Clock::now() - since).count();
}

void require(bool ok, const std::string& reason)
{
    if (!ok) throw std::runtime_error(reason);
}

std::string input(const std::string& prompt, const std::string& fallback = {})
{
    std::cout << prompt;
    if (!fallback.empty()) std::cout << " [" << fallback << "]";
    std::cout << ": ";
    std::string line;
    require(static_cast<bool>(std::getline(std::cin, line)), "input_closed");
    require(line != "q" && line != "Q", "cancelled");
    return line.empty() ? fallback : line;
}

double parse(const std::string& text)
{
    std::istringstream stream(text);
    double value = missing;
    require((stream >> value) && std::isfinite(value), "invalid_number");
    stream >> std::ws;
    require(stream.eof(), "invalid_number");
    return value;
}

double number(const std::string& prompt, double lo, double hi, const std::string& fallback = {})
{
    const auto value = parse(input(prompt, fallback));
    require(value >= lo && value <= hi, "number_out_of_range");
    return value;
}

int integer(const std::string& prompt, int lo, int hi, const std::string& fallback = {})
{
    const auto value = number(prompt, lo, hi, fallback);
    require(std::floor(value) == value, "expected_integer");
    return static_cast<int>(value);
}

bool inRange(double x) { return x >= 0 && x <= upper; }

bool usesEndpointAllowance(const std::string& phase)
{
    return phase == "A0" || phase == "B" || phase == "C";
}

bool usesEncoderMotionChecks(const std::string& phase)
{
    return phase == "A2" || phase == "B" || phase == "C" || phase == "D";
}

constexpr bool feedbackInRange(double xPulse, double endpointPulse)
{
    if (xPulse >= 0 && xPulse <= upperPulse) return true;
    return (endpointPulse == 0 || endpointPulse == upperPulse) &&
           xPulse >= endpointPulse - endpointAllowancePulse &&
           xPulse <= endpointPulse + endpointAllowancePulse;
}

static_assert(feedbackInRange(32003, upperPulse));
static_assert(feedbackInRange(32010, upperPulse));
static_assert(feedbackInRange(-10, 0));
static_assert(!feedbackInRange(32011, upperPulse));
static_assert(!feedbackInRange(-11, 0));
static_assert(!feedbackInRange(32001, 0));
static_assert(!feedbackInRange(-1, upperPulse));
static_assert(!feedbackInRange(32001, missing));

struct EndpointFeedback {
    double target = missing;
    double departure = missing;

    constexpr void beginMove(double nextTarget)
    {
        departure = target == nextTarget ? missing : target;
        target = nextTarget;
    }

    constexpr bool accept(double feedbackPulse)
    {
        if (feedbackInRange(feedbackPulse, missing)) {
            departure = missing;
            return true;
        }
        return feedbackInRange(feedbackPulse, target) || feedbackInRange(feedbackPulse, departure);
    }
};

constexpr bool endpointReversalWorks(double from, double to, double firstFeedback,
                                     double inside, double lastFeedback)
{
    EndpointFeedback feedback;
    feedback.beginMove(from);
    feedback.beginMove(to);
    return feedback.accept(firstFeedback) && !feedback.accept(from == upperPulse ? 32011 : -11) &&
           feedback.accept(inside) && !feedback.accept(firstFeedback) && feedback.accept(lastFeedback);
}

static_assert(endpointReversalWorks(upperPulse, 0, 32002, 31989, -3));
static_assert(endpointReversalWorks(0, upperPulse, -2, 11, 32003));

constexpr bool followsDirection(double progress, double furthestProgress, double tolerance)
{
    return progress >= furthestProgress - tolerance;
}

static_assert(followsDirection(23999 - 23989, 23999 - 23998, 10));
static_assert(followsDirection(9, 19, 10));
static_assert(!followsDirection(8, 19, 10));

std::vector<double> motionValues(const std::string& prompt, const std::string& fallback,
                                 std::size_t count)
{
    auto line = input(prompt, fallback);
    std::replace(line.begin(), line.end(), ',', ' ');
    std::istringstream stream(line);
    std::vector<double> values;
    for (std::string item; stream >> item;) values.push_back(parse(item));
    require(values.size() == count, "Incorrect number of motion parameters");
    return values;
}

long pulse(double x)
{
    require(inRange(x), "target_out_of_range");
    return std::lround(x * pulsePerMm);
}

std::vector<double> points(const std::string& line)
{
    require(!line.empty() && line.back() != ',', "empty_point");
    std::vector<double> values;
    std::set<long> unique;
    std::istringstream stream(line);
    for (std::string item; std::getline(stream, item, ',');) {
        const auto p = pulse(parse(item));
        require(unique.insert(p).second, "duplicate_point_after_rounding");
        values.push_back(p / pulsePerMm);
    }
    return values;
}

struct Trial {
    int id, group, direction;
    double target, approach, speed;
    std::string use;
};

struct Plan {
    std::string phase, bProtocol;
    std::vector<Trial> trials;
    double period = .01, duration = 2, window = 1, offset = 1;
    double posTol = .01, velTol = .05, acceleration = 0, transfer = 0, timeout = 30;
    double poweredSeconds = missing;
    int segments = 1;
    short loopIndex = 1, positionMode = 2;
    std::string rState = "R_fixed_operator_confirmed", config = "a33_core2_axis1";
};

void add(Plan& plan, int group, int direction, double target, double approach,
         double speed, const std::string& use)
{
    pulse(target);
    if (direction) pulse(approach);
    plan.trials.push_back({static_cast<int>(plan.trials.size()) + 1, group, direction,
                           target, approach, speed, use});
}

void makeBPlan(Plan& p, bool advanced)
{
    p.duration = 4;
    int rounds = 1, firstDirection = 1;
    long approachPulse = 1000;
    std::vector<long> nodes;
    std::string use = "boundary_single";
    const auto addNodes = [&nodes](long first, long last) {
        for (long x = first; x <= last; x += 100) nodes.push_back(x);
    };
    if (p.bProtocol == "B") {
        use = "train";
        rounds = advanced ? integer("B main complete rounds", 1, 10, "2") : 2;
        if (advanced) {
            firstDirection = integer("First sweep direction (1 or -1)", -1, 1, "1");
            require(firstDirection != 0, "First sweep direction must be 1 or -1");
        }
        addNodes(1000, 31000);
    } else if (p.bProtocol == "BL") {
        firstDirection = -1;
        addNodes(0, 1000);
    } else if (p.bProtocol == "BH") {
        addNodes(31000, 32000);
    } else {
        use = "boundary_compare";
        approachPulse = advanced ? integer("Short symmetric approach (pulses)", 100, 900, "500") : 500;
        addNodes(((approachPulse + 99) / 100) * 100, 1000);
        addNodes(31000, ((32000 - approachPulse) / 100) * 100);
    }
    p.offset = approachPulse / pulsePerMm;
    const auto sweep = [&](int group, int direction) {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const long target = direction > 0 ? nodes[i] : nodes[nodes.size() - 1 - i];
            add(p, group, direction, target / pulsePerMm,
                (target - direction * approachPulse) / pulsePerMm, p.transfer, use);
        }
    };
    for (int round = 1; round <= rounds; ++round) {
        const int direction = round % 2 ? firstDirection : -firstDirection;
        sweep(round, direction);
        if (use != "boundary_single") sweep(round, -direction);
    }
}

void makePlan(Plan& p, const std::string& entry, bool planOnly, bool advanced)
{
    if (entry == "read_state") {
        p.phase = "READ";
        p.duration = advanced ? number("Read duration (s)", 1, 3600, "5") : 5;
        p.positionMode = -1;
        p.rState = "R_unverified_read_only";
        p.trials.push_back({1, 1, 0, missing, missing, 0, "diagnostic"});
        return;
    }
    if (entry == "hold_positions") {
        p.phase = input("Protocol A0, A2, B (main), BL, BH, BE (boundary), D1, D2", "A0");
        if (p.phase == "B" || p.phase == "BL" || p.phase == "BH" || p.phase == "BE") {
            p.bProtocol = p.phase;
            p.phase = "B";
        }
    }
    const bool scan = entry == "scan_velocity";
    const bool dProtocol = p.phase == "D" || p.phase == "D1" || p.phase == "D2";
    const std::string motionCandidate = scan ? "2,400" : "2,20";
    const auto motion = motionValues(
        "Enter site-confirmed speed (mm/s), acceleration (mm/s^2), e.g. " + motionCandidate,
        planOnly || dProtocol ? motionCandidate : "", 2);
    p.transfer = motion[0];
    p.acceleration = motion[1];
    require(p.transfer >= .001 && p.transfer <= 1000 &&
            p.acceleration >= .001 && p.acceleration <= 100000, "Motion values out of range");
    if (scan) {
        p.phase = "C";
        p.period = .005;
        const auto speeds = motionValues(
            "Enter three site-confirmed scan speeds (mm/s), e.g. 2,10,40",
            planOnly ? "2,10,40" : "", 3);
        require(speeds[0] >= .001 && speeds[0] < speeds[1] &&
                speeds[1] < speeds[2] && speeds[2] <= 1000, "Scan speeds must increase and be in range");
        p.duration = .25;
        for (int group = 1; group <= 5; ++group) {
            for (int i = 0; i < 3; ++i) {
                const double speed = speeds[(group - 1 + i) % 3];
                const char* use = group <= 3 ? "development" : "scan_holdout";
                add(p, group, 1, upper, 0, speed, use);
                add(p, group, -1, 0, upper, speed, use);
            }
        }
        return;
    }
    if (entry == "hold_timeseries") {
        p.phase = "A1";
        p.duration = advanced ? number("Seconds per continuous segment", 1, 1200, "10") : 10;
        p.segments = advanced ? integer("Continuous segments", 1, 120, "3") : 3;
        const auto power = input("Seconds since power-on (Enter if unknown)");
        if (!power.empty()) {
            p.poweredSeconds = parse(power);
            require(p.poweredSeconds >= 0 && p.poweredSeconds <= 1e8, "Invalid time since power-on");
        }
        for (const auto x : points(input("A0 representative positions (comma-separated mm)")))
            add(p, static_cast<int>(p.trials.size()) + 1, 0, x, missing, p.transfer, "diagnostic");
        return;
    }
    if (p.phase == "B") {
        makeBPlan(p, advanced);
        return;
    }
    std::vector<double> nodes;
    int repeats = 1;
    std::string use = "diagnostic";
    if (p.phase == "A0") {
        nodes = {0, 8, 16, 24, 32};
    } else if (p.phase == "A2") {
        nodes = points(input("A0 representative interior positions (comma-separated mm)"));
        repeats = advanced ? integer("Independent repeats per direction", 2, 100, "5") : 5;
        p.duration = 4;
    } else if (dProtocol) {
        use = p.phase == "D2" ? "cross_time" : "unseen";
        if (p.phase == "D") use = input("Use unseen, cross_time", "unseen");
        require(use == "unseen" || use == "cross_time", "unknown_D_use");
        require(p.transfer == 2 && p.acceleration == 20, "D uses the frozen B protocol: enter 2,20");
        p.phase = "D";
        const std::vector<long> targetPulses = use == "unseen"
            ? std::vector<long>{1550,3770,5770,7770,9770,11770,13770,15770,17770,
                19770,21770,23050,25050,27050,29050,29950,30550}
            : std::vector<long>{4000,8000,12000,16000,20000,22000,28000,30000};
        for (const long x : targetPulses) {
            require(x >= 1000 && x <= 31000 && (use != "unseen" || x % 100 != 0),
                    "D target outside model domain or on B training grid");
            nodes.push_back(x / pulsePerMm);
        }
        repeats = 3;
        p.duration = 4;
    } else {
        throw std::runtime_error("unknown_protocol");
    }

    if (advanced && !dProtocol) {
        p.offset = pulse(number("Independent approach offset (mm)", .1, 16, "1")) / pulsePerMm;
        p.duration = number("Hold duration (s)", 1.1, 300, p.duration == 4 ? "4" : "2");
    }
    std::sort(nodes.begin(), nodes.end());
    for (int group = 1; group <= repeats; ++group) {
        if (p.phase == "A0") {
            for (const auto x : nodes)
                if (x != 0) add(p, group, 1, x, x - p.offset, p.transfer, use);
            for (auto it = nodes.rbegin(); it != nodes.rend(); ++it)
                if (*it != upper) add(p, group, -1, *it, *it + p.offset, p.transfer, use);
        } else {
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                const double x = nodes[(i + group - 1) % nodes.size()];
                const int first = group % 2 ? 1 : -1;
                add(p, group, first, x, x - first * p.offset, p.transfer, use);
                add(p, group, -first, x, x + first * p.offset, p.transfer, use);
            }
        }
    }
}

std::ofstream output(const fs::path& path)
{
    std::ofstream out(path);
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out << std::setprecision(12);
    return out;
}

std::string sessionId()
{
    SYSTEMTIME st{};
    GetSystemTime(&st);
    std::ostringstream s;
    s << std::setfill('0') << std::setw(4) << st.wYear << std::setw(2) << st.wMonth
      << std::setw(2) << st.wDay << 'T' << std::setw(2) << st.wHour << std::setw(2)
      << st.wMinute << std::setw(2) << st.wSecond << std::setw(3) << st.wMilliseconds
      << "Z_" << GetCurrentProcessId();
    return s.str();
}

double estimate(double distance, double speed, double acc)
{
    return distance > speed * speed / acc ? distance / speed + speed / acc
                                          : 2 * std::sqrt(distance / acc);
}

void preview(const Plan& p)
{
    std::cout << "\n" << p.phase << " | " << p.trials.size() << " trials";
    if (p.phase == "READ") {
        std::cout << " | " << p.duration << " s\n";
        return;
    }
    std::cout << " | command range [0," << upperPulse << "] pulses\n";
    if (p.phase == "B")
        std::cout << "B protocol " << p.bProtocol << " | grid 100 pulses | approach "
                  << p.offset * pulsePerMm << " pulses | hold only "
                  << p.trials.size() * p.duration / 60 << " min\n";
    if (usesEndpointAllowance(p.phase)) {
        const double tolerance = std::min(p.posTol * pulsePerMm, endpointAllowancePulse);
        std::cout << p.phase << " endpoint arrival tolerance: +/-" << tolerance
                  << " pulses; endpoint feedback allowance: " << endpointAllowancePulse << " pulses\n";
    }
    if (p.phase == "C") {
        std::set<double> checkedSpeeds;
        bool feasible = true;
        for (const auto& t : p.trials) {
            if (!checkedSpeeds.insert(t.speed).second) continue;
            const double rampDistance = t.speed * t.speed / (2 * p.acceleration);
            const double plateau = std::max(0., upper - 2 * rampDistance);
            const double crop = rampDistance + scanDerivativeSpan * t.speed;
            const double usableDistance = std::max(0., upper - 2 * crop);
            const double minimumUsable = std::max(3 * p.period * t.speed, .5);
            std::cout << "Scan " << t.speed * pulsePerMm << " pulses/s | nominal plateau "
                      << plateau * pulsePerMm << " pulses";
            if (usableDistance > 0) {
                std::cout << " | estimated usable [" << crop * pulsePerMm << ','
                          << (upper - crop) * pulsePerMm << "] pulses, length "
                          << usableDistance * pulsePerMm << " pulses";
            }
            std::cout << '\n';
            feasible = feasible && usableDistance >= minimumUsable;
        }
        require(feasible, "Insufficient constant-speed distance in the 32000-pulse stroke");
    }
    double total = 0, previous = missing;
    for (const auto& t : p.trials) {
        const double first = t.direction ? t.approach : t.target;
        const auto transfer = std::isfinite(previous) ? std::abs(first - previous)
                                                       : std::max(first, upper - first);
        total += estimate(transfer, p.transfer, p.acceleration);
        if (t.direction)
            total += estimate(std::abs(t.target - t.approach), t.speed, p.acceleration);
        total += p.duration * p.segments;
        std::cout << "trial " << t.id << " group " << t.group << " | " << t.use
                  << " | " << pulse(first) << " -> " << pulse(t.target) << " pulses | direction "
                  << t.direction << " | " << t.speed * pulsePerMm << " pulses/s\n";
        previous = t.target;
    }
    std::cout << "Estimated " << total << " s including holds, plus settling and read overhead.\n";
}

struct Sample {
    double time = 0, readTime = 0, gap = 0, x = missing, v = missing, temp = missing;
    long status = 0, planner = -1;
    short raw = 0, mode = -1, lp = 0, ln = 0;
    short rx = 0, rv = 0, ri = 0, rs = 0, rm = 0, rl = 0, rp = 0, rt = -1;
    bool valid() const { return !(rx || rv || ri || rs || rm || rl || rp) && std::isfinite(x) && std::isfinite(v); }
    bool moving() const { return (status & movingMask) != 0; }
};

void command(short rc, const char* name)
{
    if (rc) throw std::runtime_error(std::string(name) + "_rc_" + std::to_string(rc));
}

class SamplingWait {
public:
    explicit SamplingWait(bool highResolution)
    {
        if (!highResolution) return;
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
        require(timer != nullptr, "cannot_create_high_resolution_sampling_timer");
    }

    ~SamplingWait() { if (timer) CloseHandle(timer); }

    void until(Time due) const
    {
        if (!timer) {
            std::this_thread::sleep_until(due);
            return;
        }
        const auto delay = due - Clock::now();
        if (delay <= Clock::duration::zero()) return;
        using TimerTick = std::chrono::duration<LONGLONG, std::ratio<1, 10000000>>;
        LARGE_INTEGER relative{};
        relative.QuadPart = -std::chrono::ceil<TimerTick>(delay).count();
        require(SetWaitableTimer(timer, &relative, 0, nullptr, nullptr, FALSE) != 0 &&
                WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0, "sampling_timer_wait_failed");
    }

    const char* name() const { return timer ? "windows_high_resolution" : "sleep_until"; }

private:
    HANDLE timer = nullptr;
};

class Acquisition {
public:
    Acquisition(const fs::path& root, const Plan& plan)
        : p(plan), samplingWait(plan.phase == "C" || plan.period <= .005),
          session(sessionId()), start(Clock::now())
    {
        fs::create_directories(root / "data" / "raw");
        fs::create_directories(root / "manifest");
        raw = output(root / "data" / "raw" / (session + ".csv"));
        trials = output(root / "manifest" / (session + ".trials.csv"));
        meta = output(root / "manifest" / (session + ".txt"));
        auto planned = output(root / "manifest" / (session + ".plan.csv"));
        planned << "session_id,trial_id,phase,group,use,direction,target_mm,approach_mm,speed_mm_s\n";
        for (const auto& t : p.trials)
            planned << session << ',' << t.id << ',' << p.phase << ',' << t.group << ','
                    << t.use << ',' << t.direction << ',' << t.target << ',' << t.approach
                    << ',' << t.speed << '\n';
        raw << "session_id,trial_id,phase,group,use,stage,segment,direction,target_mm,speed_mm_s,"
               "t_s,read_s,gap_s,x_mm,v_mm_s,current_fb_raw,status,enabled,alarm,mode,planner,"
               "limit_pos,limit_neg,temp_card_c,powered_s,rc_pos,rc_vel,rc_current,rc_status,"
               "rc_mode,rc_limit,rc_planner,rc_temp,valid\n";
        trials << "session_id,trial_id,phase,group,use,direction,target_mm,approach_mm,start_mm,"
                  "speed_mm_s,hold_s,window_s,period_s,pos_tol_mm,vel_tol_mm_s,status,reason\n";
        meta << "session_id=" << session << "\nprogram_version=" << version
             << "\nbuild=" << __DATE__ << ' ' << __TIME__
             << "\ncore=2\naxis=1\nalias=a33\npulse_per_mm=1000\nmin_mm=0\nmax_mm=32"
             << "\ncurrent_api=GTN_GetAtlTorque\ncurrent_unit=raw\nphase=" << p.phase
             << "\nfeedback_tolerance_pulse=" << (usesEndpointAllowance(p.phase) ? endpointAllowancePulse : 0)
             << "\nconfig=" << p.config << "\nR_state=" << p.rState
             << "\nloop_index=" << p.loopIndex << "\nposition_mode=" << p.positionMode
             << "\nacc_mm_s2=" << p.acceleration << "\ntransfer_mm_s=" << p.transfer
             << "\nperiod_s=" << p.period << "\nhold_s=" << p.duration << "\nwindow_s=" << p.window
             << "\nsampling_timer=" << samplingWait.name()
             << "\noffset_mm=" << p.offset << "\nsegments=" << p.segments
             << "\npowered_s_at_start=" << p.poweredSeconds << "\ntimeout_s=" << p.timeout
             << "\npos_tol_mm=" << p.posTol << "\nvel_tol_mm_s=" << p.velTol << std::endl;
        if (usesEncoderMotionChecks(p.phase)) {
            meta << p.phase << "_direction_check=encoder_displacement\n"
                 << p.phase << "_settling_check=axis_state_and_position" << std::endl;
        }
        if (p.phase == "B") meta << "B_protocol=" << p.bProtocol << "\nB_step_pulse=100" << std::endl;
        std::cout << "Session " << session << '\n';
    }

    void run()
    {
        std::string reason = "completed";
        try {
            command(GTN_OpenCard(5, nullptr, nullptr), "GTN_OpenCard");
            opened = true;
            wchar_t dll[MAX_PATH]{};
            const auto dllSize = GetModuleFileNameW(GetModuleHandleW(L"gts.dll"), dll, MAX_PATH);
            if (dllSize && dllSize < MAX_PATH)
                meta << "sdk_dll=" << fs::path(dll).u8string() << std::endl;
            TVersion firmware{};
            const auto firmwareRc = GTN_GetVersionEx(core, 1, &firmware);
            meta << "firmware_rc=" << firmwareRc << "\nfirmware_version=";
            if (!firmwareRc) meta << firmware.version;
            meta << std::endl;
            if (p.phase != "READ") {
                double smoothDec = 0, abruptDec = 0;
                command(GTN_GetStopDec(core, axis, &smoothDec, &abruptDec), "GTN_GetStopDec");
                meta << "smooth_stop_dec_pulse_ms2=" << smoothDec
                     << "\nabrupt_stop_dec_pulse_ms2=" << abruptDec << std::endl;
                require(std::isfinite(smoothDec) && smoothDec > 0, "invalid_smooth_stop_deceleration");
            }
            for (const auto& t : p.trials) runTrial(t);
        } catch (const std::exception& e) {
            reason = e.what();
        }
        if (opened) {
            const short close = GTN_Close();
            meta << "close_rc=" << close << '\n';
            if (close && reason == "completed") reason = "close_failed";
        }
        meta << "outcome=" << reason << "\nsamples=" << count << "\nmissing=" << invalid
             << "\nlate_intervals=" << late << "\nmean_read_s=" << (count ? readSum / count : 0)
             << "\nmean_period_s=" << (intervals ? gapSum / intervals : 0)
             << "\nmax_period_s=" << maxGap << std::endl;
        std::cout << "Samples " << count << ", missing " << invalid << ", late " << late
                  << ", mean read " << (count ? readSum / count : 0)
                  << " s, mean period " << (intervals ? gapSum / intervals : 0) << " s\n";
        require(reason == "completed", reason);
    }

private:
    const Plan& p;
    SamplingWait samplingWait;
    std::string session;
    Time start, prev{}, due{};
    std::ofstream raw, trials, meta;
    bool opened = false, activeMove = false, stopping = false;
    EndpointFeedback endpoint;
    int count = 0, invalid = 0, late = 0, intervals = 0;
    double readSum = 0, gapSum = 0, maxGap = 0;

    Sample sample(const Trial& t, const char* stage, double target, int segment = 0)
    {
        samplingWait.until(due);
        const auto now = Clock::now();
        due = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(p.period));
        Sample s;
        s.time = std::chrono::duration<double>(now - start).count();
        s.gap = prev == Time{} ? 0 : std::chrono::duration<double>(now - prev).count();
        prev = now;
        double x = 0, v = 0, temperature = 0;
        s.rx = GTN_GetAxisEncPos(core, axis, &x);
        s.rv = GTN_GetAxisEncVel(core, axis, &v);
        s.ri = GTN_GetAtlTorque(core, axis, &s.raw);
        s.rs = GTN_GetSts(core, axis, &s.status);
        s.rm = GTN_GetControlLoopMode(core, axis, p.loopIndex, &s.mode);
        s.rl = GTN_GetLimitStatus(core, axis, &s.lp, &s.ln);
        s.rp = GTN_GetPrfMode(core, axis, &s.planner);
        if (p.phase == "A1" || p.phase == "READ") {
            s.rt = GTN_GetTemperature(core, &temperature);
            if (!s.rt) s.temp = temperature;
        }
        if (!s.rx) s.x = x / pulsePerMm;
        if (!s.rv) s.v = v * 1000 / pulsePerMm;
        s.readTime = seconds(now);
        raw << session << ',' << t.id << ',' << p.phase << ',' << t.group << ',' << t.use
            << ',' << stage << ',' << segment << ',' << t.direction << ',' << target
            << ',' << t.speed << ',' << s.time << ',' << s.readTime << ',' << s.gap
            << ',' << s.x << ',' << s.v << ',' << (s.ri ? missing : s.raw)
            << ',' << (s.rs ? -1 : s.status) << ',' << (s.rs ? -1 : !!(s.status & enableMask))
            << ',' << (s.rs ? -1 : !!(s.status & alarmMask)) << ',' << (s.rm ? -1 : s.mode)
            << ',' << (s.rp ? -1 : s.planner) << ',' << (s.rl ? -1 : s.lp)
            << ',' << (s.rl ? -1 : s.ln) << ',' << s.temp << ',' << p.poweredSeconds + s.time
            << ',' << s.rx << ',' << s.rv << ',' << s.ri << ',' << s.rs << ',' << s.rm
            << ',' << s.rl << ',' << s.rp << ',' << s.rt << ',' << s.valid() << '\n';
        ++count;
        invalid += !s.valid();
        late += s.gap > 1.5 * p.period;
        readSum += s.readTime;
        if (s.gap > 0) {
            ++intervals;
            gapSum += s.gap;
            maxGap = std::max(maxGap, s.gap);
        }
        if (!stopping) require(!interrupted.load(), "operator_stop");
        if (!stopping && _kbhit()) {
            const int key = _getch();
            require(key != 'q' && key != 'Q', "operator_stop");
        }
        if (p.phase != "READ" && !stopping) {
            require(s.valid(), "missing_feedback");
            require((s.status & enableMask) && !(s.status & faultMask) && !s.lp && !s.ln,
                    "axis_disabled_fault_or_limit");
            require(s.mode == p.positionMode && s.planner == PROFILE_MODE_TRAP, "unexpected_controller_mode");
            if (usesEndpointAllowance(p.phase) && t.id == 1 && std::string(stage) == "before_trial" &&
                !feedbackInRange(x, missing) && stationary(s)) {
                double plannerPulse = missing;
                command(GTN_GetPrfPos(core, axis, &plannerPulse), "GTN_GetPrfPos");
                if (feedbackInRange(x, plannerPulse)) endpoint.beginMove(plannerPulse);
            }
            require(endpoint.accept(x), "feedback_outside_work_range");
        }
        return s;
    }

    bool isProtectedEndpoint(double target) const
    {
        return usesEndpointAllowance(p.phase) && (target == 0 || target == upper);
    }

    double positionTolerance(double target) const
    {
        return isProtectedEndpoint(target) ? std::min(p.posTol, endpointAllowancePulse / pulsePerMm) : p.posTol;
    }

    bool stationary(const Sample& s) const
    {
        // Feedback velocity can briefly disagree with both encoder position and axis state.
        return !s.moving() && (usesEncoderMotionChecks(p.phase) || std::abs(s.v) <= p.velTol);
    }

    bool at(const Sample& s, double target) const
    {
        const double tolerance = positionTolerance(target);
        return stationary(s) &&
               s.x >= target - tolerance && s.x <= target + tolerance;
    }

    Sample move(const Trial& t, double target, double speed, const char* stage, const Sample& from)
    {
        const long targetPulse = pulse(target);
        require(stationary(from), "not_idle_at_command");
        if (at(from, target) && std::string(stage) == "transfer") return from;
        double plannerPosition = missing;
        command(GTN_GetPrfPos(core, axis, &plannerPosition), "GTN_GetPrfPos");
        const double plannerMm = plannerPosition / pulsePerMm;
        require(inRange(plannerMm) && from.x >= plannerMm - p.posTol && from.x <= plannerMm + p.posTol,
                "planner_feedback_position_mismatch");
        TTrapPrm trap{};
        trap.acc = trap.dec = p.acceleration * pulsePerMm / 1e6;
        command(GTN_SetTrapPrm(core, axis, &trap), "GTN_SetTrapPrm");
        command(GTN_SetPos(core, axis, targetPulse), "GTN_SetPos");
        command(GTN_SetVel(core, axis, speed * pulsePerMm / 1000), "GTN_SetVel");
        activeMove = true;
        command(GTN_Update(core, axisMask), "GTN_Update");
        endpoint.beginMove(isProtectedEndpoint(target) ? static_cast<double>(targetPulse) : missing);
        const auto begin = Clock::now();
        int consecutive = 0;
        const double direction = target > from.x ? 1 : -1;
        double furthestProgress = 0;
        while (seconds(begin) < p.timeout) {
            const auto s = sample(t, stage, target);
            const double progress = direction * (s.x - from.x);
            if (usesEncoderMotionChecks(p.phase)) {
                require(followsDirection(progress, furthestProgress, p.posTol),
                        "unexpected_motion_direction");
                furthestProgress = std::max(furthestProgress, progress);
            } else {
                require(direction * s.v >= -p.velTol && progress >= -p.posTol,
                        "unexpected_motion_direction");
            }
            consecutive = at(s, target) ? consecutive + 1 : 0;
            if (consecutive >= 3) {
                activeMove = false;
                return s;
            }
        }
        throw std::runtime_error("arrival_timeout");
    }

    void hold(const Trial& t, bool& stable)
    {
        auto origin = Clock::now();
        for (int segment = 1; segment <= p.segments; ++segment) {
            const auto end = p.duration * segment;
            auto lastStableStart = missing;
            auto last = missing;
            for (;;) {
                const auto s = sample(t, p.phase == "C" ? "turnaround" : "hold", t.target, segment);
                const double elapsed = seconds(origin);
                if (!at(s, t.target) || (std::isfinite(last) && s.time - last > 2.5 * p.period)) {
                    lastStableStart = missing;
                } else if (!std::isfinite(lastStableStart)) {
                    lastStableStart = s.time;
                }
                last = s.time;
                if (elapsed >= end) {
                    stable = stable && std::isfinite(lastStableStart) &&
                             s.time - lastStableStart >= std::min(p.window, p.duration) - 2.5 * p.period;
                    break;
                }
            }
        }
    }

    void runTrial(const Trial& t)
    {
        double actualStart = missing;
        std::string status = "completed", reason = "ok";
        try {
            if (p.phase == "READ") {
                const auto begin = Clock::now();
                do { sample(t, "read", missing); } while (seconds(begin) < p.duration);
            } else {
                const double first = t.direction ? t.approach : t.target;
                const auto before = sample(t, "before_trial", first);
                auto s = move(t, first, p.transfer, "transfer", before);
                s = sample(t, "approach_start", first);
                require(at(s, first), "approach_start_not_settled");
                actualStart = s.x;
                if (t.direction) {
                    require(t.direction * (t.target - actualStart) >= .95 * std::abs(t.target - first),
                            "invalid_approach_direction_or_distance");
                    s = move(t, t.target, t.speed, p.phase == "C" ? "scan" : "approach", s);
                    require(t.direction * (s.x - actualStart) >= .9 * std::abs(t.target - first),
                            "no_measured_directional_displacement");
                }
                bool stable = true;
                hold(t, stable);
                sample(t, "trial_end", t.target);
                if (!stable) {
                    status = "invalid";
                    reason = "unstable_hold";
                }
            }
        } catch (const std::exception& e) {
            status = "aborted";
            reason = e.what();
            if (activeMove) {
                const auto stop = GTN_Stop(core, axisMask, 0);
                activeMove = false;
                meta << "abort_trial_id=" << t.id << "\nabort_reason=" << reason
                     << "\nsmooth_stop_rc=" << stop << std::endl;
                raw.flush();
                stopping = true;
                bool stopped = false;
                try {
                    const auto stopStart = Clock::now();
                    do {
                        const auto s = sample(t, "abort_stop", t.target);
                        stopped = !s.rs && !s.rv && !s.moving() && std::abs(s.v) <= p.velTol;
                        if (stopped) break;
                    } while (seconds(stopStart) < 2);
                } catch (const std::exception&) {
                    stopped = false;
                }
                meta << "stop_feedback=" << (stopped ? "stationary" : "unconfirmed") << std::endl;
                std::cout << "Smooth stop command rc=" << stop << ". Check axis state on site.\n";
            }
        }
        raw.flush();
        trials << session << ',' << t.id << ',' << p.phase << ',' << t.group << ',' << t.use << ','
               << t.direction << ',' << t.target << ',' << t.approach << ',' << actualStart << ','
               << t.speed << ',' << p.duration << ',' << p.window << ',' << p.period << ','
               << positionTolerance(t.target) << ',' << p.velTol << ','
               << status << ',' << reason << std::endl;
        require(status != "aborted", reason);
    }
};

fs::path projectRoot()
{
    wchar_t path[32768]{};
    const auto length = GetModuleFileNameW(nullptr, path, 32768);
    require(length && length < 32768, "cannot_resolve_executable");
    for (auto dir = fs::path(path).parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::exists(dir / "ZRPositionCurrent.vcxproj")) return dir;
        if (dir == dir.root_path()) break;
    }
    throw std::runtime_error("project_root_not_found");
}
} // namespace

int runExperiment(const char* entry, int argc, char** argv)
{
    try {
        bool planOnly = false, advanced = false;
        for (int i = 1; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--plan") planOnly = true;
            else if (option == "--advanced") advanced = true;
            else throw std::runtime_error("usage: ZRPositionCurrent [--plan] [--advanced]");
        }
        std::cout << "ZRA stage 1 | core 2 axis 1 a33 | 1000 pulse/mm | q to cancel/stop\n";
        if (planOnly) std::cout << "Offline preview; motion inputs here are candidates.\n";
        Plan plan;
        makePlan(plan, entry, planOnly, advanced);
        if (advanced) {
            plan.period = number("Requested sample period (ms)", 1, 100, plan.phase == "C" ? "5" : "10") / 1000;
            plan.loopIndex = static_cast<short>(integer("Control-loop index", 0, 32767, "1"));
            plan.rState = input("R-axis state", plan.rState);
            plan.config = input("Site configuration identifier", plan.config);
            if (plan.phase != "READ") {
                plan.positionMode = static_cast<short>(integer("Position-servo mode code", 0, 32767, "2"));
                plan.posTol = number("Arrival position tolerance (mm)", .001, .05, ".01");
                plan.velTol = number("Stationary velocity tolerance (mm/s)", .001, 1, ".05");
                plan.timeout = number("Arrival timeout per move (s)", .1, 3600, "30");
            }
        }
        preview(plan);
        std::cout << "Sample " << plan.period * 1000 << " ms | loop index " << plan.loopIndex
                  << " | configuration " << plan.config << " | R " << plan.rState << '\n';
        if (plan.phase != "READ") {
            std::cout << "Transfer " << plan.transfer << " mm/s | accel/decel " << plan.acceleration
                      << " mm/s^2 | position mode " << plan.positionMode
                      << " | hold " << plan.duration << " s x " << plan.segments
                      << " | arrival tolerance " << plan.posTol << " mm, "
                      << plan.velTol << " mm/s | timeout " << plan.timeout << " s\n";
        }
        if (planOnly) return 0;
        const auto root = projectRoot();
        if (plan.phase == "D") {
            require(fs::is_regular_file(root / "models" / "position_B_primary_v1.json") &&
                    fs::is_regular_file(root / "models" / "position_BC_overlap_v1.json"),
                    "Freeze both D tables before starting collection");
        }
        if (plan.phase != "READ") {
            std::cout << "Use the already homed, enabled position servo in TRAP mode. "
                         "Confirm the displayed motion parameters and position mode, "
                         "verify raw current, R fixed, limits, braking and stopping margin. "
                         "Use Q to stop the session.\n";
        }
        const std::string token = plan.phase == "READ" ? "READ" : "START";
        require(input("Type " + token + " to start", plan.phase == "READ" ? token : "") == token,
                "cancelled");
        InterruptHandler interruptHandler;
        Acquisition acquisition(root, plan);
        acquisition.run();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
