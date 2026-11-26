#include "matched_batch.hpp"
#include <process.h>

namespace {
using namespace matched_batch;

struct Slot {
    std::string id;
    int round;
    bool accelFirst;
};

std::wstring quotePath(const fs::path& path) {
    return L"\"" + path.wstring() + L"\"";
}

int analysis(const fs::path& root, const std::vector<std::wstring>& options) {
    std::vector<std::wstring> args{L"node", quotePath(root / "scripts/analyze_d.mjs"),
                                  L"--root", quotePath(root)};
    args.insert(args.end(), options.begin(), options.end());
    std::vector<const wchar_t*> argv;
    for (const auto& value : args) argv.push_back(value.c_str());
    argv.push_back(nullptr);
    const auto result = _wspawnvp(_P_WAIT, L"node.exe", argv.data());
    require(result != -1, "cannot run Node.js D analysis");
    return static_cast<int>(result);
}

struct Protocol {
    std::string id;
    Plan plan;
    std::vector<Slot> slots;
};

Protocol readProtocol(const fs::path& root, const std::string& id) {
    require(safeId(id) && id.rfind("Z-DP_", 0) == 0, "invalid D protocol ID");
    require(analysis(root, {L"--verify", fs::path(id).wstring()}) == 0, "frozen D protocol verification failed");
    const auto f = readFields(root / "manifest/d" / (id + ".txt"));
    const auto n = [&](const char* key) { return parseNumber(f.at(key)); };
    const double center = n(f.count("trajectory_plan_pulse") ? "trajectory_plan_pulse" : "plan_pulse");
    auto plan = makePlan({mm(center), mm(n("acc_pulse_s2")), mm(n("peak_pulse_s")),
                          mm(n("match_pulse_s")), 1, 1, mm(n("eval_pulse")), 1}, 600);
    Protocol p{id, plan, {}};
    for (int i = 1; i <= integer(n("planned_pairs"), 5, 5); ++i) {
        p.slots.push_back({id + "_P" + std::to_string(i), i, i % 2 == 1});
    }
    return p;
}

std::vector<Slot> pending(const fs::path& root, const Protocol& p) {
    return pendingSlots(root, "results/d", p.id, "Z-D_", p.slots);
}

const char* firstRole(const Slot& slot) {
    return slot.accelFirst ? "accel" : "decel";
}

std::string slotFields(const Slot& slot) {
    return "planned_pair_id=" + slot.id + "\ncondition=D\nseries=validation\nhistory_condition=H1\nround=" +
           std::to_string(slot.round) + "\nfirst_role=" + firstRole(slot) + '\n';
}

bool preview(std::ostream& out, const Plan& plan, bool accelFirst, double& position,
             double reserve, double stop) {
    bool ok = true;
    for (const Run* run : {accelFirst ? &plan.acceleration : &plan.deceleration,
                          accelFirst ? &plan.deceleration : &plan.acceleration}) {
        out << run->role << "_pulse=" << run->start << " -> " << run->end << '\n';
        ok = pathCheck(out, "transfer", position, mm(run->start), transferSpeed, reserve, stop) && ok;
        ok = pathCheck(out, run->role, mm(run->start), mm(run->end), plan.recipe.peak, reserve, stop) && ok;
        out << "v0_pulse_s=" << pulse(run->matchSpeed) << ", eval platform before/after="
            << (run->evalSec - run->platformBegin) * 1000 << '/'
            << (run->platformEnd - run->evalSec) * 1000 << " ms; full 31 ms window\n";
        position = mm(run->end);
    }
    return ok;
}

int acquire(const fs::path& root, const Protocol& p, const std::vector<Slot>& slots) {
    const auto limits = numbers(line("Field-confirmed extra stop reserve and following-error limit [both pulse]: "), 2);
    const double reserve = mm(integer(limits[0], 1, 32000));
    const double follow = integer(limits[1], 1, 1000);
    const auto id = sessionId("Z-D_");
    const auto snapshot = readFields(root / "manifest/d" / (p.id + ".txt"));
    auto log = createFile(root / "manifest" / (id + ".txt"));
    log << "schema=d_validate_inertia_v1\nsession_id=" << id << "\nprotocol_id=" << p.id
        << "\npurpose=validation\ncore=2\naxis=1\nconfig=a33_core2_axis1\ndirection=1\nmodel_sha256="
        << snapshot.at("model_sha256") << "\nb_range_sha256=" << snapshot.at("b_range_sha256")
        << "\nslots_requested=" << slots.size() << "\nextra_stop_reserve_pulse=" << limits[0] << '\n';
    std::vector<std::string> ids;
    for (const auto& slot : slots) ids.push_back(slot.id);
    log << "planned_slots=" << join(ids) << '\n' << std::flush;
    std::cout << "Session: " << id << '\n';

    return executeBatch(id, log, reserve, follow, static_cast<int>(slots.size()),
        [&](std::ostream& out, double initial, double margin, double stop) {
            bool ok = true;
            for (const auto& slot : slots) {
                out << slot.id << '\n';
                ok = preview(out, p.plan, slot.accelFirst, initial, margin, stop) && ok;
            }
            return ok;
        },
        [&](Recorder& card, Axis& axisControl, Ns tick, double stop, int pair) {
            const auto& slot = slots[pair - 1];
            Preparation prep;
            prep.holdMs = 250;
            prep.metadata = slotFields(slot);
            {
                auto record = createFile(root / "manifest" / (id + "_p" + std::to_string(pair) + ".txt"));
                record << "session_id=" << id << "\npair_id=" << pair << '\n' << prep.metadata
                       << "match_pulse_s=" << pulse(p.plan.recipe.match)
                       << "\nacc_pulse_s2=" << pulse(p.plan.recipe.acc)
                       << "\neval_pulse=" << pulse(p.plan.recipe.evalCenter) << '\n' << std::flush;
                const auto& first = slot.accelFirst ? p.plan.acceleration : p.plan.deceleration;
                const auto& second = slot.accelFirst ? p.plan.deceleration : p.plan.acceleration;
                const bool complete = recordRun(card, axisControl, root, id, pair, 1, p.plan, first, tick, reserve, stop, prep) &&
                                      recordRun(card, axisControl, root, id, pair, 2, p.plan, second, tick, reserve, stop, prep);
                record << "outcome=" << (complete ? "complete" : "incomplete") << '\n';
                if (!complete) return false;
            }
            // A quality failure stops the batch before another motion. Prediction errors are not inspected here.
            return analysis(root, {L"--check-pair", fs::path(p.id).wstring(), fs::path(id).wstring(),
                                   std::to_wstring(pair)}) == 0;
        });
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const auto flag = argc > 1 ? std::wstring(argv[1]) : L"";
        if (flag == L"--status" && argc == 2) return readStatus();
        if (flag == L"--plan" && argc == 2) {
            auto position = 18.0;
            const auto plan = makePlan({18, 500, 40, 18, 1, 1, 18, 1}, 600);
            return preview(std::cout, plan, true, position, 0, 0) ? 0 : 1;
        }
        const auto root = projectRoot();
        if (flag == L"--plan" && argc == 3) {
            const auto p = readProtocol(root, fs::path(argv[2]).string());
            auto initial = p.plan.recipe.evalCenter;
            return preview(std::cout, p.plan, true, initial, 0, 0) ? 0 : 1;
        }
        if (flag == L"--protocol-d" && argc == 3) {
            require(safeId(fs::path(argv[2]).string()), "invalid source B ID");
            return analysis(root, {L"--protocol-d", argv[2]});
        }
        if (flag == L"--pending" && argc == 3) {
            for (const auto& slot : pending(root, readProtocol(root, fs::path(argv[2]).string()))) {
                std::cout << slot.id << " D H1\n";
            }
            return 0;
        }
        require(argc == 1, "use --plan, --protocol-d <B ID>, --pending <D ID> or --status");
        require(line("Mode [F] D acquisition/resume: ") == "F", "use F for D acquisition");
        const auto id = line("Frozen D protocol ID: ");
        require(safeId(id) && id.rfind("Z-DP_", 0) == 0, "invalid D protocol ID");
        ProtocolLock lock(root / "manifest/d", id);
        const auto p = readProtocol(root, id);
        auto slots = pending(root, p);
        if (slots.empty()) { std::cout << "All planned D pairs are valid.\n"; return 0; }
        const int limit = std::min(4, static_cast<int>(slots.size()));
        const auto input = line("Pending D pairs to run [maximum 4]: ");
        const int count = input.empty() ? limit : integer(parseNumber(input), 1, limit);
        slots.resize(count);
        require(SetConsoleCtrlHandler(onCtrl, TRUE) != FALSE, "cannot install stop handler");
        return acquire(root, p, slots);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
