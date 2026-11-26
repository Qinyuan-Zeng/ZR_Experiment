#include "matched_motion.hpp"
#include <map>

namespace {
using namespace matched_motion;

Recipe readRecipe() {
    std::cout << "x0_plan [mm], A [mm/s^2], V, v0 [mm/s], direction [1/-1], pairs [1..30],"
                 " x_match_eval [mm], first role [1 accel / -1 decel] [16,400,40,20,1,1,15.976,1]: ";
    std::string line;
    require(bool(std::getline(std::cin, line)), "input cancelled");
    if (line.empty()) return {};
    const auto count = std::count(line.begin(), line.end(), ',') + 1;
    require(count >= 6 && count <= 8, "Expected 6 to 8 comma-separated values");
    const auto v = numbers(line, static_cast<std::size_t>(count));
    require((v[4] == 1 || v[4] == -1) &&
            v[5] >= 1 && v[5] <= 30 && v[5] == std::trunc(v[5]), "invalid direction or pair count");
    require(v.size() < 8 || v[7] == 1 || v[7] == -1, "invalid first role");
    return {v[0], v[1], v[2], v[3], static_cast<int>(v[4]), static_cast<int>(v[5]),
            v.size() >= 7 ? v[6] : v[0], v.size() == 8 ? static_cast<int>(v[7]) : 1};
}

int runOnSite(const Plan& plan) {
    std::cout << "Enter field-confirmed extra stopping reserve [mm] and following-error limit [pulse]: ";
    std::string input;
    require(bool(std::getline(std::cin, input)), "input cancelled");
    const auto limits = numbers(input, 2);
    require(limits[0] > 0 && limits[1] >= 1 && limits[1] <= 1000, "invalid field limits");
    const auto root = projectRoot();
    const auto id = sessionId("Z-A1_");
    auto log = createFile(root / "manifest" / (id + ".txt"));
    log << "schema=a1_watch_pair_v1\nsession_id=" << id
        << "\nconfig=a33_core2_axis1\ncore=2\naxis=1\nencoder_pulse_per_mm=1000"
        << "\nmodel=position_B_primary_v1\nx0_mm=" << plan.recipe.planCenter
        << "\nx0_plan_mm=" << plan.recipe.planCenter << "\nx_match_eval_mm=" << plan.recipe.evalCenter
        << "\nacc_mm_s2=" << plan.recipe.acc << "\npeak_mm_s=" << plan.recipe.peak
        << "\nmatch_mm_s=" << plan.recipe.match << "\ndirection=" << plan.recipe.direction
        << "\npairs_requested=" << plan.recipe.pairs << "\nextra_stop_reserve_mm=" << limits[0]
        << "\nrepeat_start_role=" << (plan.recipe.firstRoleSign > 0 ? "accel" : "decel")
        << "\nposition_match_tol_mm=0.01\nspeed_target_tol_mm_s=0.5\nspeed_pair_tol_mm_s=0.5"
        << "\nfit_stability_fraction=0.1\nfit_windows_ms=15,21,31\ncurrent_windows_ms=5,9,15"
        << "\nmin_pair_count=5\nsnr_threshold=5\nci_relative_halfwidth_threshold=0.3\n" << std::flush;
    std::cout << "Session " << id << '\n';
    Recorder card(log);
    Axis motor(card, log);
    bool complete = false;
    try {
        card.connect();
        const Ns tick = verifyCorePeriod(card, log);
        motor.setFollowLimit(limits[1]);
        double smooth = 0, abrupt = 0;
        card.check("GTN_GetStopDec", GTN_GetStopDec(core, axis, &smooth, &abrupt));
        require(std::isfinite(smooth) && smooth > 0 && std::isfinite(abrupt) && abrupt > 0,
                "invalid_stop_deceleration");
        const auto initial = motor.read();
        require(initial.idle() && std::abs(mm(initial.x - initial.planX)) <= positionTol,
                "axis_not_stationary_or_planner_mismatch");
        log << "smooth_stop_dec_pulse_ms2=" << smooth << "\nabrupt_stop_dec_pulse_ms2=" << abrupt << '\n';
        const double smoothMm = smooth * 1e6 / pulsePerMm;
        const bool clearance = printPlan(std::cout, plan, mm(initial.x), limits[0], smoothMm);
        printPlan(log, plan, mm(initial.x), limits[0], smoothMm);
        require(clearance, "insufficient_stopping_reserve_from_current_position");
        if (plan.recipe.pairs > 1) {
            std::cout << "Accepted first-pair session ID for this condition: ";
            require(bool(std::getline(std::cin, input)), "qualification session ID cancelled");
            const auto first = input.find_first_not_of(" \t\r");
            const auto last = input.find_last_not_of(" \t\r");
            input = first == std::string::npos ? "" : input.substr(first, last - first + 1);
            require(input.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == std::string::npos &&
                input.rfind("Z-A1_", 0) == 0, "invalid qualification session ID");
            std::ifstream gate(root / "results/a1" / (input + "_qualification.txt"));
            std::map<std::string, std::string> fields;
            std::string line;
            while (std::getline(gate, line)) {
                const auto sep = line.find('=');
                if (sep != std::string::npos) fields[line.substr(0, sep)] = line.substr(sep + 1);
            }
            const auto gatePosition = [&](const char* key) {
                return parseNumber(fields.count(key) ? fields.at(key) : fields["x0_mm"]);
            };
            require(fields["status"] == "accepted" &&
                    gatePosition("x0_plan_mm") == plan.recipe.planCenter &&
                    gatePosition("x_match_eval_mm") == plan.recipe.evalCenter &&
                    parseNumber(fields["acc_mm_s2"]) == plan.recipe.acc &&
                    parseNumber(fields["peak_mm_s"]) == plan.recipe.peak &&
                    parseNumber(fields["match_mm_s"]) == plan.recipe.match &&
                    parseNumber(fields["direction"]) == plan.recipe.direction &&
                    fields["rules"] == "a1_watch_pair_v1", "need a valid first-pair replay of the same condition");
            log << "operator_qualification_session=" << input << '\n';
        }
        std::cout << "Confirm field limits, parking clearance, R fixed, gravity/brake setup and stop function."
                     "\nType START to execute, or press Enter to cancel: ";
        require(bool(std::getline(std::cin, input)) && input == "START" && !stopRequested(), "operator_cancelled");
        log << "operator_field_confirmation=START\n" << std::flush;
        complete = true;
        for (int pair = 1; pair <= plan.recipe.pairs && complete; ++pair) {
            const bool accelFirst = (pair % 2 != 0) == (plan.recipe.firstRoleSign > 0);
            complete = recordRun(card, motor, root, id, pair, 1, plan,
                                 accelFirst ? plan.acceleration : plan.deceleration, tick, limits[0], smoothMm) &&
                       recordRun(card, motor, root, id, pair, 2, plan,
                                 accelFirst ? plan.deceleration : plan.acceleration, tick, limits[0], smoothMm);
        }
    } catch (const std::exception& e) {
        complete = false;
        motor.abort(log);
        log << "error=" << e.what() << '\n';
        std::cerr << e.what() << '\n';
    }
    card.cleanup();
    complete = complete && card.cleanupOk;
    log << "outcome=" << (complete ? "complete" : "incomplete") << '\n' << std::flush;
    return complete ? 0 : 1;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    int status = 1;
    try {
        if (argc >= 2 && std::wstring(argv[1]) == L"--plan") {
            const bool interactive = argc == 3 && std::wstring(argv[2]) == L"--input";
            require(argc == 2 || interactive, "use --plan [--input]");
            const auto plan = makePlan(interactive ? readRecipe() : Recipe{});
            double initial = plan.recipe.planCenter, reserve = 0, smooth = 0;
            if (interactive) {
                std::cout << "Optional current position [mm], extra reserve [mm], smooth-stop decel [mm/s^2]"
                             " (Enter for theoretical preview): ";
                std::string line;
                if (std::getline(std::cin, line) && !line.empty()) {
                    const auto values = numbers(line, 3);
                    initial = values[0];
                    reserve = values[1];
                    smooth = values[2];
                    require(initial >= lower && initial <= upper && reserve > 0 && smooth > 0,
                            "invalid position or reserve inputs");
                    std::cout << "Offline inputs only. Current card and axis state are unchecked.\n";
                }
            }
            return printPlan(std::cout, plan, initial, reserve, smooth) ? 0 : 1;
        }
        require(argc == 1, "use --plan [--input] or start the interactive on-site entry");
        require(SetConsoleCtrlHandler(onCtrl, TRUE) != FALSE, "Cannot install console stop handler");
        status = runOnSite(makePlan(readRecipe()));
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
    }
    DWORD consoleMode = 0;
    if (argc == 1 && GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &consoleMode)) {
        std::cout << (status == 0 ? "A1 capture complete. Review the pair with analyze_a1.mjs."
                                  : "A1 did not complete. See the reason above.")
                  << " Press Enter to close.\n" << std::flush;
        std::string ignored;
        std::getline(std::cin, ignored);
    }
    return status;
}
