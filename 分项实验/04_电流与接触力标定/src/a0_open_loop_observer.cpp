#include "watch_capture.hpp"

namespace {
using namespace watch_capture;
constexpr short controlIndex = 1;
constexpr short adcChannel = 1;
constexpr long referencePulse = 15670;
constexpr double referenceTolerancePulse = 10.0;
constexpr auto pollWait = 10ms;

struct Reading {
    double position = 0, velocity = 0, command = 0, voltage = 0;
    long status = 0;
    short mode = 0, current = 0, dacChannel1 = 0, dacRc = -1;
    unsigned long positionClock = 0, adcClock = 0, dacClock = 0;
    TSwitchLoopParameter task{};
    std::array<short, 8> rc{};
};

Reading read() {
    Reading sample;
    sample.rc[0] = GTN_GetAxisEncPos(core, axis, &sample.position, 1, &sample.positionClock);
    sample.rc[1] = GTN_GetAxisEncVel(core, axis, &sample.velocity);
    sample.rc[2] = GTN_GetSts(core, axis, &sample.status);
    sample.rc[3] = GTN_GetControlLoopMode(core, axis, controlIndex, &sample.mode);
    sample.rc[4] = GTN_GetForceValue(core, axis, controlIndex, &sample.command);
    sample.rc[5] = GTN_GetAtlTorque(core, axis, &sample.current);
    sample.rc[6] = GTN_GetAdc(core, adcChannel, &sample.voltage, 1, &sample.adcClock);
    sample.rc[7] = GTN_GetSwitchLoopParameter(core, axis, &sample.task);
    // Physical AO readback is a diagnostic candidate. Axis-to-output mapping is unverified.
    sample.dacRc = GTN_GetDac(core, 1, &sample.dacChannel1, 1, &sample.dacClock);
    return sample;
}

bool sampleValid(const Reading& s) {
    return std::all_of(s.rc.begin(), s.rc.end(), [](short value) { return value == 0; }) &&
           std::isfinite(s.position) && std::isfinite(s.velocity) && std::isfinite(s.command) &&
           std::isfinite(s.voltage);
}

bool atReference(double position) {
    return std::isfinite(position) && std::abs(position - referencePulse) <= referenceTolerancePulse;
}

void snapshot(std::ostream& log, const Reading& s, const char* stage) {
    log << stage << ".axis_position_pulse=" << s.position
        << '\n' << stage << ".velocity_sdk=" << s.velocity
        << '\n' << stage << ".axis_status=" << s.status
        << '\n' << stage << ".loop_mode=" << s.mode
        << '\n' << stage << ".force_command_register_sdk=" << s.command
        << '\n' << stage << ".current_feedback_raw=" << s.current
        << '\n' << stage << ".ai1_voltage_v=" << s.voltage
        << '\n' << stage << ".switch_task_enable=" << s.task.enable
        << '\n' << stage << ".switch_task_mode=" << s.task.mode
        << '\n' << stage << ".switch_task_open_loop_dac=" << s.task.openLoopDac
        << '\n' << stage << ".ao1_candidate_rc=" << s.dacRc << '\n';
    if (s.dacRc == 0) log << stage << ".ao1_candidate_dac_bit=" << s.dacChannel1 << '\n';
    log << stage << ".sample_valid=" << sampleValid(s)
        << '\n' << stage << ".position_in_15670_band=" << atReference(s.position) << '\n' << std::flush;
}

void readDrivePdo(std::ostream& log, std::ostream& console) {
    // GVN programming manual, instruction 184. Keep these diagnostic bytes
    // separate from the feedback current and the GVN open-loop DAC register.
    for (const auto object : std::array<unsigned short, 2>{0x6061, 0x6071}) {
        std::array<unsigned char, 256> bytes{};
        const short rc = GTN_GetEcatAxisPdoData(core, axis, object, bytes.data());
        std::ostringstream record;
        record << "pdo_" << std::hex << object << std::dec << ".rc=" << rc << '\n';
        if (rc == 0) {
            record << "pdo_" << std::hex << object << ".bytes_le=" << std::setfill('0')
                   << std::setw(2) << static_cast<unsigned>(bytes[0]);
            if (object == 0x6071) record << ',' << std::setw(2) << static_cast<unsigned>(bytes[1]);
            record << '\n';
        }
        log << record.str() << std::flush;
        console << record.str() << std::flush;
    }
}

int run(int seconds, bool transfer = false) {
    const auto root = projectRoot(L"ZRForceAcquisition.vcxproj");
    const auto id = sessionId("Z-FA0-OLREAD_");
    auto log = createFile(root / L"manifest" / (id + ".txt"));
    log << "session=" << id << "\nprotocol=Z-FA0-OPEN-15670-OBSERVE"
        << "\nobservation_type=" << (transfer ? "transfer" : "reference")
        << "\nbuild=" << __DATE__ << ' ' << __TIME__
        << "\ncore=" << core << "\naxis=" << axis << "\ncontrol_index=" << controlIndex
        << "\nadc_channel=" << adcChannel << "\nrequested_seconds=" << seconds
        << "\nreference_pulse=" << referencePulse << "\nband_pulse=" << referenceTolerancePulse
        << "\nsampling=sequential_sdk_calls\ncontroller_operations=read_only\n" << std::flush;

    Recorder card(log);
    std::string outcome = "incomplete";
    try {
        card.connect();
        double outputMax = 0;
        long overtime = 0;
        short protectionStatus = 0;
        const short protectionRc = GTN_GetLoopOutputProtect(core, axis, &outputMax, &overtime, &protectionStatus);
        log << "GTN_GetLoopOutputProtect.rc=" << protectionRc;
        if (!protectionRc) {
            log << "\nloop_output_protect_value_sdk=" << outputMax
                << "\nloop_output_protect_overtime_sdk=" << overtime
                << "\nloop_output_protect_status_sdk=" << protectionStatus;
        }
        log << '\n';
        short positive = 0, negative = 0;
        const short limitRc = GTN_GetLimitStatus(core, axis, &positive, &negative);
        log << "GTN_GetLimitStatus.rc=" << limitRc;
        if (!limitRc) log << "\npositive_limit=" << positive << "\nnegative_limit=" << negative;
        log << '\n';
        readDrivePdo(log, std::cout);
        const auto initial = read();
        snapshot(log, initial, "initial");
        snapshot(std::cout, initial, "initial");
        log << "initial.rc_position_velocity_status_mode_command_current_adc_switch=";
        for (std::size_t i = 0; i < initial.rc.size(); ++i) {
            if (i) log << ',';
            log << initial.rc[i];
        }
        log << '\n' << std::flush;
        require(sampleValid(initial) && !limitRc, "One or more SDK state reads failed");
        if (!seconds) {
            outcome = "state_read_ok";
        } else {
            require(atReference(initial.position), "Observer must start within 15670 +/- 10 pulse");
            auto csv = createFile(root / L"data/raw" / (id + ".open_loop_observe.csv"));
            csv << "host_begin_us,host_end_us,position_pulse,position_clock,velocity_sdk,axis_status,loop_mode,"
                   "force_command_register_sdk,current_feedback_raw,ai1_voltage_v,adc_clock,switch_enable,switch_mode,"
                   "position_in_reference_band,position_rc,velocity_rc,status_rc,mode_rc,command_rc,current_rc,adc_rc,switch_rc,"
                   "switch_open_loop_dac,ao1_candidate_dac_bit,ao1_candidate_clock,ao1_candidate_rc\n";
            const auto origin = Clock::now();
            const auto deadline = origin + std::chrono::seconds(seconds);
            const auto us = [origin](auto time) {
                return std::chrono::duration_cast<std::chrono::microseconds>(time - origin).count();
            };
            std::size_t rows = 0, outside = 0, invalid = 0;
            while (Clock::now() < deadline && !stopRequested()) {
                const auto begin = Clock::now();
                const auto s = read();
                const auto end = Clock::now();
                const bool inBand = !s.rc[0] && atReference(s.position);
                outside += !inBand;
                invalid += !sampleValid(s);
                ++rows;
                csv << us(begin) << ',' << us(end) << ',' << s.position << ',' << s.positionClock << ','
                    << s.velocity << ',' << s.status << ',' << s.mode << ',' << s.command << ',' << s.current
                    << ',' << s.voltage << ',' << s.adcClock << ',' << s.task.enable << ',' << s.task.mode
                    << ',' << inBand;
                for (const short value : s.rc) csv << ',' << value;
                csv << ',' << s.task.openLoopDac << ',';
                if (s.dacRc == 0) csv << s.dacChannel1 << ',' << s.dacClock;
                else csv << ',';
                csv << ',' << s.dacRc << '\n';
                if (!sampleValid(s)) break;
                std::this_thread::sleep_until(std::min(deadline, begin + pollWait));
            }
            csv.flush();
            log << "samples=" << rows << "\ninvalid_samples=" << invalid
                << "\noutside_reference_band=" << outside << '\n';
            if (rows > 0 && !invalid && !cancelled.load())
                outcome = transfer ? "recorded_open_loop_transfer_trace" :
                    (outside ? "recorded_with_position_drift" : "observed_at_reference");
        }
    } catch (const std::exception& error) {
        log << "error=" << error.what() << '\n';
        std::cerr << error.what() << '\n';
    }
    card.cleanup();
    if (!card.cleanupOk) outcome = "cleanup_failed";
    log << "outcome=" << outcome << '\n' << std::flush;
    std::cout << id << ' ' << outcome << '\n';
    return outcome == "state_read_ok" || outcome == "observed_at_reference" ||
           outcome == "recorded_open_loop_transfer_trace" ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") {
            require(atReference(15660) && atReference(15680) && !atReference(15681) &&
                    !atReference(std::numeric_limits<double>::quiet_NaN()), "Reference band test failed");
            Reading sample{};
            require(sampleValid(sample), "Valid mock sample rejected");
            sample.rc[5] = 1;
            require(!sampleValid(sample), "Invalid current read accepted");
            std::cout << "A0 read-only observer self-test passed. No SDK calls.\n";
            return 0;
        }
        if (argc == 2 && std::string(argv[1]) == "--state") return run(0);
        const std::string option = argc > 1 ? argv[1] : "";
        if (argc == 3 && (option == "--observe" || option == "--transfer")) {
            const bool transfer = option == "--transfer";
            const auto seconds = parseNumber(argv[2]);
            require(seconds == std::trunc(seconds) && seconds >= 1 && seconds <= (transfer ? 60 : 20),
                    transfer ? "Transfer duration must be an integer from 1 to 60 seconds" :
                               "Observation duration must be an integer from 1 to 20 seconds");
            require(SetConsoleCtrlHandler(onCtrl, TRUE), "Cannot install console cancellation handler");
            return run(static_cast<int>(seconds), transfer);
        }
        std::cout << "Usage: ZRForceOpenLoopObserver.exe --state | --observe <seconds> | "
                     "--transfer <seconds> | --self-test\n";
        return argc == 1 ? 0 : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
