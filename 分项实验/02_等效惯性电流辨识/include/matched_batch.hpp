#pragma once

#include "matched_motion.hpp"
#include <map>
#include <set>

namespace matched_batch {
using namespace matched_motion;
using Fields = std::map<std::string, std::string>;

inline std::string line(const char* prompt) {
    std::cout << prompt << std::flush;
    std::string value;
    require(bool(std::getline(std::cin, value)), "input cancelled");
    const auto first = value.find_first_not_of(" \t\r");
    return first == std::string::npos ? "" : value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}
inline bool safeId(const std::string& s) {
    return !s.empty() && s.size() <= 100 && s.find_first_not_of(
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-") == std::string::npos;
}
inline Fields readFields(const fs::path& path) {
    std::ifstream in(path);
    require(bool(in), "required protocol, qualification or replay file is missing");
    Fields fields;
    std::string value;
    while (std::getline(in, value)) {
        const auto split = value.find('=');
        if (split != std::string::npos) fields[value.substr(0, split)] = value.substr(split + 1);
    }
    return fields;
}
inline std::vector<std::string> split(const std::string& text) {
    std::istringstream stream(text);
    std::vector<std::string> fields;
    std::string value;
    while (std::getline(stream, value, ',')) fields.push_back(value);
    return fields;
}
inline std::string join(const std::vector<std::string>& values) {
    std::string result;
    for (const auto& value : values) result += (result.empty() ? "" : ",") + value;
    return result;
}
inline int integer(double value, int minimum, int maximum) {
    require(value >= minimum && value <= maximum && value == std::trunc(value), "number outside allowed integer range");
    return static_cast<int>(value);
}
class ProtocolLock {
public:
    ProtocolLock(const fs::path& directory, const std::string& id) {
        lock = CreateFileW((directory / (id + ".lock")).c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        require(lock != INVALID_HANDLE_VALUE, "another process is using this protocol");
    }
    ~ProtocolLock() { CloseHandle(lock); }
    ProtocolLock(const ProtocolLock&) = delete;
    ProtocolLock& operator=(const ProtocolLock&) = delete;
private:
    HANDLE lock = INVALID_HANDLE_VALUE;
};

template<class Slot>
std::vector<Slot> pendingSlots(const fs::path& root, const fs::path& resultDir, const std::string& id,
                               const std::string& sessionPrefix, const std::vector<Slot>& all) {
    std::vector<std::string> seen;
    for (const auto& file : fs::directory_iterator(root / "manifest")) {
        const auto stem = file.path().stem().string();
        if (!file.is_regular_file() || file.path().extension() != ".txt" ||
            stem.rfind(sessionPrefix, 0) != 0 || stem.find("_p") != std::string::npos) continue;
        const auto f = readFields(file.path());
        if (f.count("protocol_id") && f.at("protocol_id") == id &&
            f.count("operator_field_confirmation") && f.at("operator_field_confirmation") == "START") {
            seen.push_back(stem + ":" + std::to_string(file.file_size()));
        }
    }
    if (seen.empty()) return all;
    std::sort(seen.begin(), seen.end());
    const auto state = readFields(root / resultDir / (id + "_resume.txt"));
    require(state.at("protocol_id") == id && state.at("seen_revision") == join(seen),
            "resume receipt is stale; rerun the protocol analysis");
    const auto done = split(state.at("accepted_slots"));
    const std::set<std::string> marked(done.begin(), done.end());
    require(marked.size() == done.size() && std::all_of(done.begin(), done.end(), [&](const auto& name) {
        return std::any_of(all.begin(), all.end(), [&](const auto& slot) { return slot.id == name; });
    }), "resume receipt has an invalid slot");
    std::vector<Slot> remaining;
    for (const auto& slot : all) if (!marked.count(slot.id)) remaining.push_back(slot);
    return remaining;
}

template<class Preview, class AcquirePair>
int executeBatch(const std::string& id, std::ostream& log, double reserve, double follow,
                 int count, Preview preview, AcquirePair acquirePair) {
    Recorder card(log);
    Axis axisControl(card, log);
    bool complete = false;
    try {
        card.connect();
        const auto tick = verifyCorePeriod(card, log);
        axisControl.setFollowLimit(follow);
        double smooth = 0, abrupt = 0;
        card.check("GTN_GetStopDec", GTN_GetStopDec(core, axis, &smooth, &abrupt));
        require(std::isfinite(smooth) && smooth > 0 && std::isfinite(abrupt) && abrupt > 0,
                "invalid stop-deceleration readback");
        log << "smooth_stop_dec_pulse_ms2=" << smooth << "\nabrupt_stop_dec_pulse_ms2=" << abrupt << '\n';
        const auto initial = axisControl.read();
        require(initial.idle() && std::abs(mm(initial.x - initial.planX)) <= positionTol,
                "axis must be idle and planner/encoder aligned");
        const double stopMm = smooth * 1e6 / pulsePerMm;
        require(preview(std::cout, mm(initial.x), reserve, stopMm),
                "insufficient stopping reserve; check current position and all paths");
        preview(log, mm(initial.x), reserve, stopMm);
        require(line("Confirm servo ready, limits, R fixed, brake and verified stop reserve. Type START: ") == "START"
                && !stopRequested(), "operator cancelled");
        log << "operator_field_confirmation=START\n" << std::flush;
        complete = true;
        for (int pair = 1; pair <= count; ++pair) {
            if (!acquirePair(card, axisControl, tick, stopMm, pair)) {
                complete = false;
                break;
            }
        }
    } catch (const std::exception& error) {
        axisControl.abort(log);
        complete = false;
        log << "error=" << error.what() << '\n';
        std::cerr << error.what() << '\n';
    }
    card.cleanup();
    complete = complete && card.cleanupOk;
    log << "outcome=" << (complete ? "complete" : "incomplete") << '\n' << std::flush;
    std::cout << id << ' ' << (complete ? "acquisition complete" : "acquisition incomplete") << '\n';
    return complete ? 0 : 1;
}

inline int readStatus() {
    Recorder card(std::cerr);
    Axis motor(card, std::cerr);
    std::cout << "core=2\naxis=1\n";
    try {
        card.connect();
        motor.setFollowLimit(500);
        TailWindow stable;
        const auto deadline = Clock::now() + 1s;
        for (;;) {
            const State s = motor.feedback();
            std::ostringstream fields;
            fields << std::setprecision(15)
                   << "position_pulse=" << s.x << "\nplanned_pulse=" << s.planX
                   << "\nvelocity_pulse_s=" << s.v * 1000
                   << "\nplanned_velocity_pulse_s=" << s.planV * 1000
                   << "\nfollowing_error_pulse=" << s.error << "\nstatus_word=" << s.status
                   << "\nenabled=" << bool(s.status & enableMask) << "\nmoving=" << bool(s.status & movingMask)
                   << "\nfault=" << bool(s.status & faultMask) << "\nlimit_plus=" << s.plus
                   << "\nlimit_minus=" << s.minus << "\nmode=" << s.mode << "\nplanner=" << s.planner << '\n';
            try {
                motor.validate(s);
                require(s.idle() && std::abs(s.planV) <= velocityTol &&
                        std::abs(mm(s.x - s.planX)) <= positionTol, "not_idle_or_not_aligned");
                require(Clock::now() < deadline, "stationary_window_timeout");
            } catch (...) { std::cout << fields.str(); throw; }
            const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now().time_since_epoch()).count();
            if (stable.extend(nowUs, s.x)) { std::cout << fields.str(); break; }
            std::this_thread::sleep_for(5ms);
        }
        card.cleanup();
        require(card.cleanupOk, "close_failed");
        std::cout << "ready=1\n";
        return 0;
    } catch (const std::exception& e) {
        card.cleanup();
        std::cout << "ready=0\nreason=" << e.what() << '\n';
        return 1;
    }
}
}  // namespace matched_batch
