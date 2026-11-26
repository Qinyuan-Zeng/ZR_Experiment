#include "matched_batch.hpp"

namespace {
using namespace matched_batch;
constexpr char rules[] = "b_matched_inertia_v1";
constexpr char modelHash[] = "21a8c53ef63cd84bc91f85aaad10bac79e87fbef3671c0e71bc9360c45658050";
constexpr double preparationMs = 250;
constexpr char defaultGates[] = "A1_B1,Z-B_20261125-154322_513_46064,"
                                "Z-B_20261125-151029_707_30000,Z-B_20261125-153114_600_29040";

struct Condition {
    std::string id;
    double acceleration, match;
};
const std::vector<Condition> matrix{
    {"B1", 400, 20}, {"B2", 400, 16}, {"B3", 600, 20}, {"B4", 600, 16}
};
struct Slot {
    std::string id, series, history;
    int condition = 0, round = 0;
    bool accelFirst = true;
};
struct Protocol {
    std::string id;
    int direction = 1, rounds = 5, historyBlocks = 0;
    long approach = 0, excursion = 0;
    double historySpeed = transferSpeed;
    bool expanded = false;
    int rangeMin = 0, rangeMax = 0;
    std::vector<std::string> gateSources;
    std::vector<long> evaluation{15976, 16000, 16000, 16000};
    std::vector<std::string> gates = std::vector<std::string>(4);
    std::vector<Condition> conditions = matrix;
};

Protocol expandedProtocol(const fs::path& root) {
    std::ifstream in(root / "b_expanded_conditions.csv");
    std::string row;
    require(bool(std::getline(in, row)) && row == "condition,acc_pulse_s2,match_pulse_s",
            "invalid expanded condition table header");
    Protocol p;
    p.expanded = true;
    p.conditions.clear();
    int previousAcc = 0;
    while (std::getline(in, row)) {
        const auto fields = split(row);
        require(fields.size() == 3, "invalid expanded condition row");
        const int acc = integer(parseNumber(fields[1]), 200000, 800000);
        const int speed = integer(parseNumber(fields[2]), 16000, 20000);
        const bool first = p.conditions.size() % 2 == 0;
        require(acc % 1000 == 0 && speed == (first ? 16000 : 20000) &&
                    (first ? acc > previousAcc : acc == previousAcc) &&
                    fields[0] == "X" + std::to_string(acc / 1000) + "V" + std::to_string(speed / 1000),
                "expanded table requires increasing acceleration tiers with one 16000 and one 20000 cell");
        p.conditions.push_back({fields[0], mm(acc), mm(speed)});
        previousAcc = acc;
    }
    require(p.conditions.size() == 24, "expected 24 expanded conditions");
    p.evaluation.assign(p.conditions.size(), 16000);
    p.gates.resize(p.conditions.size());
    return p;
}

Protocol rangeProtocol(const fs::path& root, int low, int high) {
    auto p = expandedProtocol(root);
    const auto hasTier = [&](int tier) {
        return std::any_of(p.conditions.begin(), p.conditions.end(), [&](const auto& c) {
            return pulse(c.acceleration) == tier;
        });
    };
    require(low < high && hasTier(low) && hasTier(high), "range endpoints must be distinct tiers from the table");
    p.conditions.erase(std::remove_if(p.conditions.begin(), p.conditions.end(), [&](const auto& c) {
        return pulse(c.acceleration) < low || pulse(c.acceleration) > high;
    }), p.conditions.end());
    p.evaluation.resize(p.conditions.size());
    p.gates.resize(p.conditions.size());
    p.rangeMin = low;
    p.rangeMax = high;
    return p;
}

const char* legacyLevel(const std::string& condition, const std::string& gate) {
    if (condition == "B3") return "repeat";
    if (condition == "B1" && gate == "A1_B1") return "baseline";
    return "independent";
}

std::string recommendedRangeGate(const std::string& condition) {
    if (condition == "X300V16") return "Z-B_20261125-172146_540_14012";
    if (condition == "X650V16") return "Z-B_20261125-185458_295_12724";
    if (condition == "X400V20") return "A1_B1";
    if (condition == "X400V16") return "Z-B_20261125-154322_513_46064";
    if (condition == "X600V20") return "Z-B_20261125-151029_707_30000";
    if (condition == "X600V16") return "Z-B_20261125-153114_600_29040";
    return "";
}

int conditionIndex(const Protocol& p, const std::string& condition) {
    const auto it = std::find_if(p.conditions.begin(), p.conditions.end(), [&](const auto& c) {
        return c.id == condition;
    });
    require(it != p.conditions.end(), "unknown condition ID for this matrix");
    return static_cast<int>(it - p.conditions.begin());
}
double optionalNumber(const char* prompt, double defaultValue) {
    const auto value = line(prompt);
    return value.empty() ? defaultValue : parseNumber(value);
}
Plan planFor(const Protocol& p, int index) {
    const auto& condition = p.conditions[index];
    return makePlan({16, condition.acceleration, 40, condition.match, p.direction, 1,
                     mm(p.evaluation[index]), 1}, p.expanded ? 800 : 600);
}
void writeRules(std::ostream& out) {
    out << "rules=" << rules << "\nmodel=position_B_primary_v1\nmodel_sha256=" << modelHash
        << "\nx0_plan_pulse=16000\npeak_pulse_s=40000\nscale=1000"
        << "\nfit_windows_ms=15,21,31\ncurrent_windows_ms=5,9,15\ncurrent_shift_ms=0"
        << "\nposition_tol_mm=0.01\nspeed_target_tol_mm_s=0.5\nspeed_pair_tol_mm_s=0.5"
        << "\nfit_stability_fraction=0.1\nrepeat_min_pairs=5\nsnr_min=5\nci_rel_half_max=0.30"
        << "\nhistory_x_tol_pulse=10\nhistory_speed_tol_pulse_s=500\nhistory_acc_fraction=0.10"
        << "\npreparation_hold_ms=" << preparationMs << '\n';
}

std::vector<Slot> schedule(const Protocol& p) {
    std::vector<Slot> slots;
    const int count = static_cast<int>(p.conditions.size());
    for (int round = 1; round <= p.rounds; ++round) {
        for (int k = 0; k < count; ++k) {
            const int order = (round - 1 + k) % count;
            const int c = !p.rangeMin ? order : order % 2 == 0 ? order / 2 : count - 1 - order / 2;
            slots.push_back({p.id + "_M" + std::to_string(round) + "_" + p.conditions[c].id,
                            "matrix", "H1", c, round, round % 2 != 0});
        }
    }
    for (int block = 1; block <= p.historyBlocks; ++block) {
        for (int k = 0; k < 2; ++k) {
            const std::string history = (block + k) % 2 ? "H1" : "H2";
            slots.push_back({p.id + "_H" + std::to_string(block) + "_" + history, "history", history,
                             0, block, block % 2 != 0});
        }
    }
    return slots;
}

std::string planCsv(const Protocol& p, const std::vector<Slot>& slots) {
    std::ostringstream out;
    out << "planned_pair_id,condition,round,series,history_condition,first_role\n";
    for (const auto& s : slots) out << s.id << ',' << p.conditions[s.condition].id << ',' << s.round
        << ',' << s.series << ',' << s.history << ',' << (s.accelFirst ? "accel" : "decel") << '\n';
    return out.str();
}

Preparation prepare(const Protocol& protocol, const Slot& slot, const Run& run) {
    Preparation prep;
    prep.holdMs = preparationMs;
    if (slot.series == "history") {
        const long approach = run.start - protocol.direction * protocol.approach;
        prep.speed = protocol.historySpeed;
        prep.waypoints.push_back(approach);
        if (slot.history == "H2") {
            prep.waypoints.push_back(approach - protocol.direction * protocol.excursion);
            prep.waypoints.push_back(approach);
        }
    }
    prep.metadata = "planned_pair_id=" + slot.id + "\ncondition=" + protocol.conditions[slot.condition].id +
                    "\nseries=" + slot.series + "\nhistory_condition=" + slot.history +
                    "\nround=" + std::to_string(slot.round) + "\nfirst_role=" +
                    (slot.accelFirst ? "accel\n" : "decel\n");
    return prep;
}

bool printSchedule(std::ostream& out, const Protocol& protocol, const std::vector<Slot>& slots,
                   double initial, double reserve, double stop) {
    out << (reserve > 0 ? "Reserve checked against supplied/read-back values.\n"
                        : "Theoretical only; field confirmation and stopping reserve pending.\n");
    double position = initial;
    bool ok = true;
    for (const auto& slot : slots) {
        const auto plan = planFor(protocol, slot.condition);
        out << slot.id << ' ' << protocol.conditions[slot.condition].id << ' ' << slot.series << ' ' << slot.history
            << " eval=" << pulse(plan.recipe.evalCenter) << " pulse, A=" << plan.recipe.acc * 1000
            << " pulse/s^2, v0=" << plan.recipe.match * 1000 << " pulse/s, V=40000 pulse/s\n";
        for (const Run* run : {slot.accelFirst ? &plan.acceleration : &plan.deceleration,
                              slot.accelFirst ? &plan.deceleration : &plan.acceleration}) {
            const auto prep = prepare(protocol, slot, *run);
            auto points = prep.waypoints;
            points.push_back(run->start);
            for (long point : points) {
                out << "  transfer_pulse=" << pulse(position) << " -> " << point << '\n';
                ok = pathCheck(out, "  transfer", position, mm(point), prep.speed, reserve, stop) && ok;
                position = mm(point);
            }
            out << "  " << run->role << "_pulse=" << run->start << " -> " << run->end << '\n';
            ok = pathCheck(out, run->role, mm(run->start), mm(run->end), plan.recipe.peak, reserve, stop) && ok;
            const auto before = run->evalSec - run->platformBegin;
            const auto after = run->platformEnd - run->evalSec;
            const auto evalSpeed = plan.recipe.acc * (run->role[0] == 'a' ? before : after);
            out << "  ideal v(eval)=" << protocol.direction * evalSpeed * 1000
                << " pulse/s; eval platform before/after=" << before * 1000 << '/' << after * 1000
                << " ms; full windows 15/21/31 ms:";
            for (int width : {15, 21, 31}) out << ' ' << (2000 * std::min(before, after) > width ? "YES" : "NO");
            out << '\n';
            position = mm(run->end);
        }
    }
    return ok;
}

Fields readGate(const fs::path& root, const std::string& gate, const char* level, const Condition& condition, int direction,
                bool allowLegacy = false) {
    require(safeId(gate), "invalid qualification ID");
    const auto f = readFields(root / "results/b" / (gate + "_gate.txt"));
    const std::string context = std::string(condition.id) + " qualification " + gate;
    const auto expect = [&](bool matches, const std::string& message) {
        require(matches, (context + ": " + message).c_str());
    };
    const auto& source = f.at("condition");
    if (allowLegacy && source != condition.id) {
        const auto old = std::find_if(matrix.begin(), matrix.end(), [&](const auto& cell) {
            return cell.id == source && cell.acceleration == condition.acceleration && cell.match == condition.match;
        });
        expect(old != matrix.end(), "original B qualification does not match this cell");
        level = legacyLevel(source, gate);
    }
    expect(f.at("status") == "accepted" && f.at("rules") == rules && f.at("model_sha256") == modelHash &&
               (source == condition.id || allowLegacy) && !f.at("evidence").empty(),
           "status, rules, position model, condition or evidence mismatch");
    expect(f.at("level") == level, std::string("requires accepted ") + level + " qualification");
    expect(parseNumber(f.at("direction")) == direction && parseNumber(f.at("x0_plan_pulse")) == 16000 &&
               parseNumber(f.at("peak_pulse_s")) == 40000,
           "direction, planning center or peak speed mismatch");
    expect(parseNumber(f.at("match_pulse_s")) == condition.match * 1000,
           "matching speed " + f.at("match_pulse_s") + " differs from current matrix " +
               std::to_string(pulse(condition.match)) + " pulse/s");
    expect(parseNumber(f.at("acc_pulse_s2")) == condition.acceleration * 1000,
           "acceleration differs from current matrix");
    return f;
}

void assertGate(const fs::path& root, const std::string& gate, const char* level,
                int c, const Protocol& p) {
    const auto f = readGate(root, gate, level, p.conditions[c], p.direction);
    require(parseNumber(f.at("eval_pulse")) == p.evaluation[c],
            "qualification does not cover this exact evaluation point");
}

void loadFormalGates(const fs::path& root, Protocol& p, const Fields* frozen = nullptr) {
    p.gateSources.resize(p.conditions.size());
    for (std::size_t i = 0; i < p.conditions.size(); ++i) {
        const auto& gate = p.gates[i];
        const auto* level = legacyLevel(p.conditions[i].id, gate);
        const auto f = readGate(root, gate, level, p.conditions[i], p.direction, p.rangeMin > 0);
        p.gateSources[i] = f.at("condition");
        p.evaluation[i] = integer(parseNumber(f.at("eval_pulse")), 1000, 31000);
        p.conditions[i].match = mm(parseNumber(f.at("match_pulse_s")));
        p.conditions[i].acceleration = mm(parseNumber(f.at("acc_pulse_s2")));
        if (frozen) {
            const std::string c = p.conditions[i].id;
            require(!p.rangeMin || frozen->at(c + "_gate_condition") == p.gateSources[i],
                    "frozen qualification source changed");
            require(parseNumber(frozen->at(c + "_eval_pulse")) == p.evaluation[i],
                    "frozen evaluation point differs from qualification");
            const auto speed = frozen->find(c + "_match_pulse_s");
            const auto acc = frozen->find(c + "_acc_pulse_s2");
            if (p.expanded || speed != frozen->end() || acc != frozen->end() || frozen->count("d_plan_pulse")) {
                require(speed != frozen->end() && acc != frozen->end() &&
                            parseNumber(speed->second) == pulse(p.conditions[i].match) &&
                            parseNumber(acc->second) == pulse(p.conditions[i].acceleration),
                        "frozen motion parameters differ from qualification");
            }
        }
    }
}

Plan heldOutPlan(const Protocol& p, long speed, long acc, long center, long eval) {
    require(std::none_of(p.conditions.begin(), p.conditions.end(), [&](const auto& c) {
        return pulse(c.acceleration) == acc && pulse(c.match) == speed;
    }), "D must reserve a held-out speed/acceleration combination");
    require(std::find(p.evaluation.begin(), p.evaluation.end(), eval) == p.evaluation.end(),
            "D must reserve a position outside the B evaluation points");
    const auto plan = makePlan({mm(center), mm(acc), 40, mm(speed), p.direction, 1, mm(eval), 1}, 600);
    const auto accelSpeed = acc * (plan.acceleration.evalSec - plan.acceleration.platformBegin);
    const auto decelSpeed = acc * (plan.deceleration.platformEnd - plan.deceleration.evalSec);
    require(std::abs(accelSpeed - speed) <= 500 && std::abs(decelSpeed - speed) <= 500 &&
                std::abs(accelSpeed - decelSpeed) <= 500,
            "D ideal speeds at evaluation exceed the target or pair tolerance");
    return plan;
}

void newProtocol(const fs::path& root, Protocol p) {
    const bool expanded = p.expanded;
    if (expanded) {
        for (std::size_t i = 0; i < p.conditions.size(); ++i) {
            const auto suggested = p.rangeMin ? recommendedRangeGate(p.conditions[i].id) : "";
            const auto prompt = p.conditions[i].id + " qualification ID" +
                                (suggested.empty() ? ": " : " [" + suggested + "]: ");
            p.gates[i] = line(prompt.c_str());
            if (p.gates[i].empty()) p.gates[i] = suggested;
            require(safeId(p.gates[i]), "each expanded condition needs a qualification ID");
        }
    } else {
        const auto gatePrompt = std::string("Gate IDs for B1,B2,B3,B4 [") + defaultGates + "]: ";
        const auto gate = line(gatePrompt.c_str());
        const auto gates = split(gate.empty() ? defaultGates : gate);
        require(gates.size() == 4, "enter four gate IDs");
        p.gates = gates;
    }
    p.direction = integer(optionalNumber("Direction [1/-1] [1]: ", 1), -1, 1);
    require(p.direction != 0, "direction must be 1 or -1");
    loadFormalGates(root, p);
    std::cout << "Qualified matrix: direction=" << p.direction << ", plan=16000 pulse, peak=40000 pulse/s\n"
              << "Condition | Qualification | Level | Eval (pulse) | Match (pulse/s) | Acc (pulse/s^2)\n";
    for (std::size_t i = 0; i < p.conditions.size(); ++i) {
        const auto& c = p.conditions[i];
        const auto* level = legacyLevel(p.gateSources[i], p.gates[i]);
        std::cout << c.id << " | " << p.gates[i] << " | " << level << " | " << p.evaluation[i]
                  << " | " << pulse(c.match) << " | " << pulse(c.acceleration) << '\n';
    }
    if (!expanded) {
        p.rounds = integer(optionalNumber("H1 matrix rounds [5]: ", 5), 5, 30);
        p.historyBlocks = integer(optionalNumber("Extra paired B1 history blocks [0, or 2..20]: ", 0), 0, 20);
    }
    require(p.historyBlocks != 1, "history contrast requires at least two blocks");
    if (p.historyBlocks) {
        const auto v = numbers(line("Field-confirmed final approach, H2 excursion [pulse], pre-move speed [pulse/s]: "), 3);
        p.approach = integer(v[0], 1, 10000);
        p.excursion = integer(v[1], 1, 10000);
        p.historySpeed = mm(integer(v[2], 500, 2000));
    }
    const auto d = numbers(line("D held-out speed [pulse/s], acceleration [pulse/s^2], planning center, evaluation [pulse]: "), 4);
    const int dSpeed = integer(d[0], 1, 39999), dAcc = integer(d[1], 1, 600000);
    const int dCenter = integer(d[2], 1000, 31000), dPos = integer(d[3], 1000, 31000);
    const auto dPlan = heldOutPlan(p, dSpeed, dAcc, dCenter, dPos);
    const auto dTime = line("D independent session/time label (required): ");
    const auto threshold = numbers(line("D mean bias absolute limit, RMSE limit [raw], minimum valid independent pairs: "), 3);
    require(!dTime.empty() && threshold[0] > 0 && threshold[1] > 0, "D time and positive thresholds required");
    const auto dCount = integer(threshold[2], 1, 1000);
    p.id = sessionId("Z-BP_");
    const auto slots = schedule(p);
    const auto matrixPairs = p.rounds * p.conditions.size();
    std::cout << "Main H1 matrix: " << p.rounds << " rounds, " << matrixPairs << " pairs, "
              << matrixPairs * 2 << " measured runs. Additional history: " << p.historyBlocks * 2 << " pairs.\n";
    require(printSchedule(std::cout, p, slots, 16, 0, 0), "protocol has a theoretical boundary failure");
    std::cout << "D held-out TRAP preview: planning center=" << dCenter << " pulse, evaluation=" << dPos
              << " pulse. Both 31 ms windows fit.\n";
    require(printPlan(std::cout, dPlan, mm(dCenter), 0, 0, "D"), "D has a theoretical boundary failure");
    std::cout << "D independent session: " << dTime << ", absolute mean bias <= " << threshold[0]
              << " raw, RMSE <= " << threshold[1] << " raw, minimum valid pairs=" << dCount << '\n';
    require(line("Type SAVE to freeze this protocol (offline): ") == "SAVE", "protocol cancelled");
    auto out = createFile(root / "manifest/b" / (p.id + ".txt"));
    out << "protocol_id=" << p.id << "\ndirection=" << p.direction << "\nrounds=" << p.rounds
        << "\nhistory_blocks=" << p.historyBlocks << "\nhistory_approach_pulse=" << p.approach
        << "\nhistory_excursion_pulse=" << p.excursion << "\nhistory_speed_mm_s=" << p.historySpeed
        << "\nd_speed_pulse_s=" << dSpeed << "\nd_acc_pulse_s2=" << dAcc
        << "\nd_plan_pulse=" << dCenter << "\nd_eval_pulse=" << dPos
        << "\nd_time=" << dTime << "\nd_max_abs_bias_raw=" << threshold[0] << "\nd_max_rmse_raw="
        << threshold[1] << "\nd_min_pairs=" << dCount << '\n';
    writeRules(out);
    if (p.rangeMin) out << "matrix_design=expanded_range_v1\n"
                        << "range_min_acc_pulse_s2=" << p.rangeMin << "\nrange_max_acc_pulse_s2=" << p.rangeMax
                        << "\nmax_batch_pairs=4\n";
    else if (expanded) out << "matrix_design=expanded_12x2\n";
    for (std::size_t i = 0; i < p.conditions.size(); ++i) {
        const auto& c = p.conditions[i];
        out << c.id << "_eval_pulse=" << p.evaluation[i] << '\n'
            << c.id << "_match_pulse_s=" << pulse(c.match) << '\n'
            << c.id << "_acc_pulse_s2=" << pulse(c.acceleration) << '\n'
            << c.id << "_gate=" << p.gates[i] << '\n';
        if (p.rangeMin) out << c.id << "_gate_condition=" << p.gateSources[i] << '\n';
    }
    auto plan = createFile(root / "manifest/b" / (p.id + ".plan.csv"));
    plan << planCsv(p, slots);
    out.flush();
    plan.flush();
    std::cout << "Frozen protocol: " << p.id << '\n';
}

Protocol readProtocol(const fs::path& root, const std::string& id) {
    require(safeId(id) && id.rfind("Z-BP_", 0) == 0, "invalid protocol ID");
    const auto f = readFields(root / "manifest/b" / (id + ".txt"));
    require(f.at("protocol_id") == id && f.at("rules") == rules && f.at("model_sha256") == modelHash,
            "protocol identity or model mismatch");
    const auto design = f.find("matrix_design");
    const bool ranged = design != f.end() && design->second == "expanded_range_v1";
    require(design == f.end() || design->second == "expanded_12x2" || ranged, "unknown frozen matrix design");
    Protocol p;
    if (ranged) {
        require(f.at("max_batch_pairs") == "4", "invalid maximum batch size");
        p = rangeProtocol(root, integer(parseNumber(f.at("range_min_acc_pulse_s2")), 200000, 800000),
                                integer(parseNumber(f.at("range_max_acc_pulse_s2")), 200000, 800000));
    } else if (design != f.end()) {
        p = expandedProtocol(root);
    }
    p.id = id;
    p.direction = integer(parseNumber(f.at("direction")), -1, 1);
    p.rounds = integer(parseNumber(f.at("rounds")), 5, 30);
    p.historyBlocks = integer(parseNumber(f.at("history_blocks")), 0, 20);
    p.approach = integer(parseNumber(f.at("history_approach_pulse")), 0, 10000);
    p.excursion = integer(parseNumber(f.at("history_excursion_pulse")), 0, 10000);
    p.historySpeed = parseNumber(f.at("history_speed_mm_s"));
    require(p.direction != 0 && p.historyBlocks != 1 && p.historySpeed >= .5 && p.historySpeed <= 2 &&
            (!p.historyBlocks || (p.approach > 0 && p.excursion > 0)), "invalid frozen history configuration");
    for (std::size_t i = 0; i < p.conditions.size(); ++i) {
        const std::string c = p.conditions[i].id;
        p.gates[i] = f.at(c + "_gate");
    }
    require(!p.expanded || (p.rounds == 5 && p.historyBlocks == 0), "invalid expanded matrix counts");
    loadFormalGates(root, p, &f);
    if (f.count("d_plan_pulse")) {
        heldOutPlan(p, integer(parseNumber(f.at("d_speed_pulse_s")), 1, 39999),
                    integer(parseNumber(f.at("d_acc_pulse_s2")), 1, 600000),
                    integer(parseNumber(f.at("d_plan_pulse")), 1000, 31000),
                    integer(parseNumber(f.at("d_eval_pulse")), 1000, 31000));
        require(!f.at("d_time").empty() && parseNumber(f.at("d_max_abs_bias_raw")) > 0 &&
                    parseNumber(f.at("d_max_rmse_raw")) > 0,
                "invalid D acceptance criteria");
        integer(parseNumber(f.at("d_min_pairs")), 1, 1000);
    }
    std::ifstream frozen(root / "manifest/b" / (id + ".plan.csv"));
    std::ostringstream saved;
    saved << frozen.rdbuf();
    require(frozen.is_open() && saved.str() == planCsv(p, schedule(p)), "missing or altered frozen plan");
    return p;
}

std::vector<Slot> pending(const fs::path& root, const Protocol& p) {
    return pendingSlots(root, "results/b", p.id, "Z-B_", schedule(p));
}

void pairMetadata(std::ostream& out, const std::string& session, int pair, const Slot& slot,
                  const Protocol& p) {
    out << "session_id=" << session << "\npair_id=" << pair << "\nplanned_pair_id=" << slot.id
        << "\ncondition=" << p.conditions[slot.condition].id << "\nround=" << slot.round
        << "\nseries=" << slot.series << "\nhistory_condition=" << slot.history
        << "\nfirst_role=" << (slot.accelFirst ? "accel" : "decel")
        << "\nmatch_pulse_s=" << pulse(p.conditions[slot.condition].match)
        << "\nacc_pulse_s2=" << pulse(p.conditions[slot.condition].acceleration)
        << "\neval_pulse=" << p.evaluation[slot.condition] << '\n';
}

int acquire(const fs::path& root, const char* purpose, const Protocol& p,
            const std::vector<Slot>& slots, const std::string& qualification) {
    if (slots.empty()) { std::cout << "All planned slots are valid.\n"; return 0; }
    const auto limits = numbers(line("Field-confirmed extra stop reserve and following-error limit [both pulse]: "), 2);
    const double reserve = mm(integer(limits[0], 1, 32000));
    const double follow = integer(limits[1], 1, 1000);
    const auto id = sessionId("Z-B_");
    auto log = createFile(root / "manifest" / (id + ".txt"));
    log << "schema=" << rules << "\nsession_id=" << id << "\npurpose=" << purpose
        << "\nprotocol_id=" << p.id << "\nqualification_id=" << qualification
        << "\ncore=2\naxis=1\nconfig=a33_core2_axis1\ndirection=" << p.direction
        << "\nslots_requested=" << slots.size() << "\nextra_stop_reserve_pulse=" << limits[0] << '\n';
    writeRules(log);
    std::vector<std::string> slotIds;
    for (const auto& slot : slots) slotIds.push_back(slot.id);
    log << "planned_slots=" << join(slotIds) << '\n' << std::flush;
    std::cout << "Session: " << id << '\n';
    const auto preview = [&](std::ostream& out, double initial, double extra, double stop) {
        return printSchedule(out, p, slots, initial, extra, stop);
    };
    return executeBatch(id, log, reserve, follow, static_cast<int>(slots.size()), preview,
        [&](Recorder& card, Axis& axisControl, Ns tick, double stopMm, int pair) {
            const auto& slot = slots[pair - 1];
            auto record = createFile(root / "manifest" / (id + "_p" + std::to_string(pair) + ".txt"));
            pairMetadata(record, id, pair, slot, p);
            record.flush();
            const auto plan = planFor(p, slot.condition);
            const auto& first = slot.accelFirst ? plan.acceleration : plan.deceleration;
            const auto& second = slot.accelFirst ? plan.deceleration : plan.acceleration;
            const bool complete = recordRun(card, axisControl, root, id, pair, 1, plan, first, tick, reserve, stopMm,
                                 prepare(p, slot, first)) &&
                       recordRun(card, axisControl, root, id, pair, 2, plan, second, tick, reserve, stopMm,
                                 prepare(p, slot, second));
            record << "outcome=" << (complete ? "complete" : "incomplete") << '\n' << std::flush;
            return complete;
        });
}

int interactive(const fs::path& root) {
    const auto input = line("Mode [Q] qualification first pair, R qualification repeats, F formal/resume: ");
    const auto mode = input.empty() ? "Q" : input;
    if (mode == "F") {
        const auto p = readProtocol(root, line("Frozen B protocol ID: "));
        ProtocolLock lock(root / "manifest/b", p.id);
        const auto remaining = pending(root, p);
        for (const auto& slot : remaining) std::cout << "Pending " << slot.id << '\n';
        if (remaining.empty()) return 0;
        const auto waiting = static_cast<int>(remaining.size());
        const auto limit = p.rangeMin ? std::min(4, waiting) : waiting;
        const auto prompt = p.rangeMin ? "Pending slots to run [maximum 4]: " : "Pending slots to run [all]: ";
        const auto count = integer(optionalNumber(prompt, limit), 1, limit);
        return acquire(root, "formal", p, {remaining.begin(), remaining.begin() + count}, "");
    }
    require(mode == "Q" || mode == "R", "use Q, R or F");
    const auto choice = line("Condition B1..B4 or expanded X###V## [B3]: ");
    Protocol p = !choice.empty() && choice.front() == 'X' ? expandedProtocol(root) : Protocol{};
    const int c = conditionIndex(p, choice.empty() ? "B3" : choice);
    p.evaluation[c] = integer(optionalNumber(!p.expanded && c == 0 ? "Evaluation position [pulse] [15976]: "
                                                   : "Evaluation position [pulse] [16000]: ",
                                             p.evaluation[c]), 1000, 31000);
    p.direction = integer(optionalNumber("Direction [1/-1] [1]: ", 1), -1, 1);
    require(p.direction != 0, "direction must be 1 or -1");
    planFor(p, c);
    int count = 1;
    std::string qualification;
    const char* purpose = "diagnostic";
    if (mode == "R") {
        purpose = "qual_repeat";
        count = integer(optionalNumber("Qualification repeat pairs for this session [5]: ", 5), 1, 30);
        qualification = line("Independent first-pair B qualification ID: ");
        assertGate(root, qualification, "independent", c, p);
    } else {
        const auto type = line("First pair purpose [D diagnostic / I independent] [D]: ");
        require(type.empty() || type == "D" || type == "I", "use D or I");
        if (type == "I") purpose = "independent";
    }
    std::vector<Slot> slots;
    for (int i = 1; i <= count; ++i) slots.push_back({"Q" + std::to_string(i), "qualification", "H1", c, i, i % 2 != 0});
    return acquire(root, purpose, p, slots, qualification);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    int result = 1;
    try {
        const auto flag = argc > 1 ? std::wstring(argv[1]) : L"";
        if (flag == L"--status" && argc == 2) return readStatus();
        if (flag == L"--plan" || flag == L"--plan-expanded") {
            require(argc <= 3, "use --plan or --plan-expanded [condition ID|--input]");
            const auto p = flag == L"--plan-expanded" ? expandedProtocol(projectRoot()) : Protocol{};
            double initial = 16, reserve = 0, stop = 0;
            std::vector<Slot> slots;
            if (argc == 3 && std::wstring(argv[2]) != L"--input") {
                const int c = conditionIndex(p, fs::path(argv[2]).string());
                slots.push_back({"Preview", "matrix", "H1", c, 1, true});
            } else {
                for (int c = 0; c < static_cast<int>(p.conditions.size()); ++c) {
                    slots.push_back({"Preview", "matrix", "H1", c, 1, true});
                }
                if (argc == 3) {
                    const auto value = line("Offline start, extra reserve [pulse], smooth stop [pulse/s^2] (Enter skips): ");
                    if (!value.empty()) {
                        const auto v = numbers(value, 3);
                        initial = mm(integer(v[0], 0, 32000));
                        reserve = mm(integer(v[1], 1, 32000));
                        stop = mm(v[2]);
                        require(stop > 0, "positive stop deceleration required");
                    }
                }
            }
            return printSchedule(std::cout, p, slots, initial, reserve, stop) ? 0 : 1;
        }
        const auto root = projectRoot();
        if (flag == L"--draft-expanded" && argc == 2) {
            auto p = expandedProtocol(root);
            p.id = "DRAFT_EXPANDED";
            std::cout << planCsv(p, schedule(p));
            return 0;
        }
        if (flag == L"--draft-range" && (argc == 2 || argc == 4)) {
            const int low = argc == 4 ? integer(parseNumber(fs::path(argv[2]).string()), 200000, 800000) : 300000;
            const int high = argc == 4 ? integer(parseNumber(fs::path(argv[3]).string()), 200000, 800000) : 650000;
            auto p = rangeProtocol(root, low, high);
            p.id = "DRAFT_RANGE";
            std::cout << planCsv(p, schedule(p));
            return 0;
        }
        if (flag == L"--protocol-range" && argc == 2) {
            const auto input = line("First and last acceleration tier [300000,650000]: ");
            const auto values = input.empty() ? std::vector<double>{300000, 650000} : numbers(input, 2);
            newProtocol(root, rangeProtocol(root, integer(values[0], 200000, 800000),
                                                 integer(values[1], 200000, 800000)));
            return 0;
        }
        if ((flag == L"--protocol" || flag == L"--protocol-expanded") && argc == 2) {
            newProtocol(root, flag == L"--protocol-expanded" ? expandedProtocol(root) : Protocol{});
            return 0;
        }
        if (flag == L"--pending" && argc == 3) {
            const auto p = readProtocol(root, fs::path(argv[2]).string());
            for (const auto& slot : pending(root, p)) {
                std::cout << slot.id << ' ' << p.conditions[slot.condition].id << ' ' << slot.history << '\n';
            }
            return 0;
        }
        require(argc == 1, "use --plan, --plan-expanded, --draft-expanded, --draft-range, --protocol, --protocol-expanded, --protocol-range, --pending <id> or interactive entry");
        require(SetConsoleCtrlHandler(onCtrl, TRUE) != FALSE, "Cannot install stop handler");
        result = interactive(root);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    }
    DWORD mode = 0;
    if (argc == 1 && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode)) {
        std::cout << "Press Enter to close.\n" << std::flush;
        std::string ignored;
        std::getline(std::cin, ignored);
    }
    return result;
}
