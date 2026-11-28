#include "watch_capture.hpp"

namespace {
using namespace watch_capture;

constexpr short adcChannel = 1;
constexpr double historicalZeroV = 0.0;
constexpr double historicalNPerV = 9.81;
constexpr auto pollPeriod = 10ms;
constexpr std::size_t channelCount = 5;

using Columns = std::array<std::vector<double>, channelCount>;

Tick decodeTick(double value) {
    require(std::isfinite(value) && value >= -2147483648.0 && value <= 4294967295.0 &&
            value == std::trunc(value), "Invalid Watch tick");
    return static_cast<Tick>(static_cast<std::int64_t>(value));
}

struct FrameCheck {
    Tick span;
    std::size_t discontinuities;
};

FrameCheck checkFrames(const Columns& columns, Tick before, Tick after) {
    const auto count = columns.front().size();
    const Tick bracket = after - before;
    require(count >= 2 && bracket < 0x80000000u, "Insufficient Watch frames or invalid bracket");
    for (const auto& column : columns) {
        require(column.size() == count, "Watch column lengths differ");
        require(std::all_of(column.begin(), column.end(),
                            [](double v) { return std::isfinite(v); }), "Nonfinite Watch value");
    }
    const Tick first = decodeTick(columns.front().front());
    Tick previous = first;
    require(first - before <= bracket, "First Watch tick outside bracket");
    std::size_t discontinuities = 0;
    for (std::size_t i = 1; i < count; ++i) {
        const Tick current = decodeTick(columns[0][i]);
        require(current - before <= bracket, "Watch tick outside bracket");
        if (current - previous != 1) ++discontinuities;
        previous = current;
    }
    for (const double currentRaw : columns[3]) {
        require(currentRaw >= -32768 && currentRaw <= 32767 &&
                currentRaw == std::trunc(currentRaw), "Invalid signed Watch current");
    }
    return {previous - first, discontinuities};
}

void selfTest() {
    Columns columns{{{4294967294.0, -1, 0, 1}, {10, 10, 10, 10}, {0, 0, 0, 0},
                     {-1, 0, 1, -2}, {-5, 0, 7, 12}}};
    auto checked = checkFrames(columns, 4294967293u, 2);
    require(checked.span == 3 && checked.discontinuities == 0, "Wraparound test failed");
    columns[0].back() = 2;
    checked = checkFrames(columns, 4294967293u, 2);
    require(checked.span == 4 && checked.discontinuities == 1, "Gap test failed");
    columns.back().pop_back();
    bool rejected = false;
    try {
        checkFrames(columns, 4294967293u, 2);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "Unequal columns test failed");
    std::cout << "A0 offline self-test passed. No SDK calls.\n";
}

std::int64_t hostUs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
}

bool pollAdc(std::ostream& raw, Clock::time_point start, std::size_t index) {
    unsigned long voltageClock = 0, adcClock = 0;
    double volts = std::numeric_limits<double>::quiet_NaN();
    short adc = 0;
    const auto voltageBegin = hostUs(start);
    const short voltageRc = GTN_GetAdc(core, adcChannel, &volts, 1, &voltageClock);
    const auto voltageEnd = hostUs(start);
    const auto rawBegin = hostUs(start);
    const short rawRc = GTN_GetAdcValue(core, adcChannel, &adc, 1, &adcClock);
    const auto rawEnd = hostUs(start);
    const bool voltageValid = voltageRc == 0 && std::isfinite(volts);
    raw << index << ',' << voltageBegin << ',' << voltageEnd << ',' << rawBegin << ',' << rawEnd << ',';
    if (voltageValid) raw << volts;
    raw << ',';
    if (rawRc == 0) raw << adc;
    raw << ',';
    if (voltageValid) raw << (volts - historicalZeroV) * historicalNPerV;
    raw << ',';
    if (voltageRc == 0) raw << voltageClock;
    raw << ',';
    if (rawRc == 0) raw << adcClock;
    raw << ',' << voltageRc << ',' << rawRc << '\n' << std::flush;
    return voltageValid && rawRc == 0;
}

void axisReference(Recorder& recorder, std::ostream& log, const std::string& label) {
    double position = 0, velocity = 0;
    short current = 0;
    unsigned long positionClock = 0, velocityClock = 0;
    recorder.check(label + ".GTN_GetEncPos", GTN_GetEncPos(core, axis, &position, 1, &positionClock));
    recorder.check(label + ".GTN_GetEncVel", GTN_GetEncVel(core, axis, &velocity, 1, &velocityClock));
    recorder.check(label + ".GTN_GetAtlTorque", GTN_GetAtlTorque(core, axis, &current));
    log << label << ".position_pulse=" << position << '\n'
        << label << ".position_clock_raw=" << positionClock << '\n'
        << label << ".velocity_sdk_value=" << velocity << '\n'
        << label << ".velocity_clock_raw=" << velocityClock << '\n'
        << label << ".current_feedback_raw=" << current << '\n' << std::flush;
}

void saveWatch(std::ostream& raw, const Columns& columns) {
    raw << "frame,card_tick_sdk,encoder_position_raw,encoder_velocity_sdk,current_feedback_raw,ai_sdk\n";
    std::size_t rows = 0;
    for (const auto& column : columns) rows = std::max(rows, column.size());
    for (std::size_t i = 0; i < rows; ++i) {
        raw << i;
        for (const auto& column : columns) {
            raw << ',';
            if (i < column.size()) raw << column[i];
        }
        raw << '\n';
    }
    raw.flush();
}

int collect(bool useWatch, int seconds) {
    const auto root = projectRoot(L"ZRForceAcquisition.vcxproj");
    const auto id = sessionId("Z-FA0_");
    const auto data = root / L"data/raw";
    auto log = createFile(root / L"manifest" / (id + ".txt"));
    auto adc = createFile(data / (id + ".adc.csv"));
    adc << "sample,host_voltage_begin_us,host_voltage_end_us,host_raw_begin_us,host_raw_end_us,"
           "voltage_v,adc_raw,force_legacy_n,voltage_clock_raw,adc_clock_raw,voltage_rc,adc_rc\n";
    log << "session=" << id << "\nprotocol=stage4_A0\nbuild=" << __DATE__ << ' ' << __TIME__
        << "\nmode=" << (useWatch ? "watch_ai_candidate_with_adc_poll" : "adc_poll")
        << "\nconnection=PCIe_5\ncore=" << core << "\naxis=" << axis
        << "\nadc_api=GTN_GetAdc/GTN_GetAdcValue\nadc_channel=" << adcChannel
        << "\nseconds_requested=" << seconds << "\npoll_requested_ms=" << pollPeriod.count()
        << "\nhistorical_zero_v=" << historicalZeroV << "\nhistorical_n_per_v=" << historicalNPerV
        << "\nforce_column_status=historical_conversion_pending_hardware_check"
        << "\noperator_confirmation=START\n" << std::flush;
    std::cout << "会话 " << id << '\n';
    const std::array<TWatchVar, channelCount> watchChannels{{
        {WATCH_VAR_PRF_LOOP, 1, 1}, {WATCH_VAR_ENC_POS, axis, 1},
        {WATCH_VAR_ENC_VEL, axis, 1}, {WATCH_VAR_ATL_TORQUE, axis, 1},
        {WATCH_VAR_AI, adcChannel, 1}
    }};
    Recorder recorder(log);
    std::string outcome = "incomplete";
    std::size_t samples = 0;
    int exitCode = 1;
    try {
        require(!stopRequested(), "Cancelled before connecting");
        recorder.connect();
        const auto origin = Clock::now();
        require(pollAdc(adc, origin, samples++), "Initial ADC read failed; inspect the poll CSV");
        double filter = std::numeric_limits<double>::quiet_NaN();
        const short filterRc = GTN_GetAdcFilterPrm(core, adcChannel, &filter);
        log << "GTN_GetAdcFilterPrm.rc=" << filterRc << '\n';
        if (filterRc == 0 && std::isfinite(filter)) log << "adc_filter_coefficient=" << filter << '\n';
        auto start = Clock::now();
        unsigned long capacity = 0;
        Ns tickNs = 0;
        CardClock before{};
        if (useWatch) {
            tickNs = verifyCorePeriod(recorder, log);
            axisReference(recorder, log, "before");
            const auto requested = std::ceil((seconds + 0.5) * 1e9 / static_cast<double>(tickNs));
            require(requested <= maxReadCapacity, "Watch host capacity exceeded");
            capacity = static_cast<unsigned long>(requested);
            log << "watch_interval=" << interval << "\nwatch_count=" << watchCount
                << "\nread_capacity_per_channel=" << capacity << "\noverflow_flag=unavailable"
                << "\nwatch_layout_fields=length,fraction,format,hex,sign,bit\n";
            require(!stopRequested(), "Cancelled before Watch configuration");
            recorder.configure(watchChannels);
            log << "watch_before.host_begin_us=" << hostUs(origin) << '\n';
            before = readClock(recorder, log, "watch_before");
            log << "watch_before.host_end_us=" << hostUs(origin) << '\n';
            require(!stopRequested(), "Cancelled before starting Watch");
            recorder.start();
            start = Clock::now();
            std::this_thread::sleep_for(10ms);
            const auto running = recorder.info("started");
            require(running.run && running.interval == interval && running.mode == WATCH_MODE_STATIC &&
                    running.varCount == watchChannels.size(), "Watch start readback mismatch");
        }
        std::cout << "采集中。Q / Ctrl+C 结束。\n" << std::flush;
        const auto deadline = start + std::chrono::seconds(seconds);
        bool adcOk = true;
        while (Clock::now() < deadline && !stopRequested()) {
            const auto next = Clock::now() + pollPeriod;
            if (!pollAdc(adc, origin, samples++)) {
                adcOk = false;
                break;
            }
            std::this_thread::sleep_until(std::min(next, deadline));
        }

        if (useWatch) {
            recorder.stop();
            log << "watch_after.host_begin_us=" << hostUs(origin) << '\n';
            const auto after = readClock(recorder, log, "watch_after");
            log << "watch_after.host_end_us=" << hostUs(origin) << '\n';
            const auto frozen = recorder.info("stopped");
            require(!frozen.run && frozen.varCount == watchChannels.size(), "Watch did not freeze");
            auto raw = createFile(data / (id + ".watch.csv"));
            auto batches = createFile(data / (id + ".batches.csv"));
            batches << "column,channel_code,host_read_begin_us,host_read_end_us,frames,rc,overflow\n";
            const auto captured = readColumns(log, batches, origin, capacity, watchChannels);
            saveWatch(raw, captured.columns);
            const auto readback = recorder.info("after_read");
            require(!readback.run && readback.time == frozen.time && readback.head == frozen.head &&
                    readback.varCount == frozen.varCount && captured.aligned,
                    "Watch columns empty, unequal, truncated, unstable or read error");
            axisReference(recorder, log, "after");
            const auto stats = checkFrames(captured.columns, before.tick, after.tick);
            const auto spanNs = stats.span * tickNs;
            log << "watch_frames=" << captured.columns.front().size()
                << "\nwatch_span_ns=" << spanNs << "\nwatch_discontinuities=" << stats.discontinuities
                << "\nwatch_clock_bracket_ok=1\n" << std::flush;
            if (!cancelled.load() && adcOk) {
                require(stats.discontinuities == 0, "Watch tick discontinuities");
                require(spanNs >= seconds * 1000000000LL - 2 * tickNs, "Watch duration too short");
            }
        }
        require(adcOk, "ADC read failed during capture; inspect the poll CSV");
        if (!cancelled.load()) {
            outcome = useWatch ? "watch_structure_ok_ai_semantics_pending" : "adc_read_ok";
            exitCode = 0;
        }
    } catch (const std::exception& error) {
        log << "error=" << error.what() << '\n' << std::flush;
        std::cerr << error.what() << '\n';
    }
    recorder.cleanup();
    if (cancelled.load()) outcome = "operator_cancelled";
    if (!recorder.cleanupOk) outcome = "cleanup_failed";
    log << "adc_attempted_samples=" << samples << "\noutcome=" << outcome << '\n' << std::flush;
    std::cout << outcome << '\n';
    return recorder.cleanupOk && !cancelled.load() ? exitCode : 1;
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--self-test") {
            selfTest();
            return 0;
        }
        if (argc != 1) {
            std::cerr << "Usage: ZRForceAcquisition.exe [--self-test]\n";
            return 2;
        }

        std::cout << "第四阶段 A0，核 2 / ADC AI1 / Z 轴 a33。\n"
                     "1 普通 ADC，2 同组 Watch 候选 + 普通 ADC，回车选 1，Q 退出 > ";
        std::string input;
        if (!std::getline(std::cin, input) || input == "Q" || input == "q") return 0;
        require(input.empty() || input == "1" || input == "2", "Invalid capture mode");
        const bool useWatch = input == "2";
        const int defaultSeconds = useWatch ? 5 : 15;
        std::cout << "时长，整数 1～" << (useWatch ? 10 : 60) << " 秒，回车采用 "
                  << defaultSeconds << " 秒 > ";
        if (!std::getline(std::cin, input) || input == "Q" || input == "q") return 0;
        const double seconds = input.empty() ? defaultSeconds : parseNumber(input);
        require(seconds == std::trunc(seconds) && seconds >= 1 && seconds <= (useWatch ? 10 : 60),
                "Invalid duration");
        std::cout << "AI1，普通 ADC 请求间隔 10 ms，采集 " << seconds << " 秒。\n";
        if (useWatch) std::cout << "Watch 会清除并替换空闲 Timer 组的现有配置和数据，请先保存需保留的记录。\n";
        std::cout << "确认当前接线与会话条件后输入 START，其他输入退出 > ";
        if (!std::getline(std::cin, input) || input != "START") return 0;
        require(SetConsoleCtrlHandler(onCtrl, TRUE), "Cannot install console cancellation handler");
        return collect(useWatch, static_cast<int>(seconds));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
