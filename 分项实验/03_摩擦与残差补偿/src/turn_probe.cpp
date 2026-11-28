#include "matched_batch.hpp"
#include <deque>

namespace {
using namespace matched_batch;

struct Limits { double lo,hi,reserve,follow,stop,response; short table; };
class Guard {
public:
    explicit Guard(const Limits& limits, std::ostream& stream) : l(limits), log(stream) {}
    double cap = 50;

    void check(const State& s)
    {
        require(s.x >= l.lo && s.x <= l.hi, "closeout_feedback_outside_boundary");
        if (!positions.empty()) {
            const auto dt = s.encoderClock - positions.back().first;
            require(dt < 0x80000000UL, "closeout_encoder_clock_reversed");
            // Submillisecond observations can differ in position at an equal SDK timestamp.
            if (dt == 0) {
                require(std::abs(s.x - firstInTick) <= cap + 2, "closeout_encoder_jump_within_one_ms");
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
        unsigned long velocityClock = 0;
        double sdkVelocity = 0;
        const auto begin = Axis::hostUs(Clock::now());
        const auto rc = GTN_GetAxisEncVel(core, axis, &sdkVelocity, 1, &velocityClock);
        const auto end = Axis::hostUs(Clock::now());
        log << end << ',' << s.encoderClock << ',' << s.x << ','
            << s.planV << ',' << speed << ',' << s.error << ',' << s.status << ',' << begin << ',' << end << ',' << sdkVelocity << ',' << velocityClock << ',' << rc << '\n';
        require(std::abs(s.planV) <= cap && speed <= cap, "closeout_velocity_protection_exceeded");
    }

private:
    const Limits& l;
    std::ostream& log;
    std::deque<std::pair<unsigned long, double>> positions;
    double firstInTick = 0;
};

template<class Read>
void hold(Read read, Recorder& card, std::ostream& log,
          long target, int durationMs, const char* role)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(durationMs + 600);
    Clock::time_point began{}, previous{};
    double lo = 0, hi = 0;
    while (Clock::now() < deadline) {
        require(!stopRequested(), "operator_stop");
        const auto s = read();
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
    throw std::runtime_error("closeout_continuous_hold_not_established");
}

struct Knot { double t,x,v; };
struct Route { const char* id; long start, apex, end; double v1,v2,a1,a2; int tail; };
constexpr std::array<Route,4> routes{{
    {"SB-1",15000,17000,15000,10,10,400,400,1000},
    {"SB-2",17000,15000,17000,10,10,400,400,1000},
    {"E2-original",10500,21500,15500,38,12,600,400,3000},
    {"E2-mirror",21500,10500,16500,38,12,600,400,3000},
}};

std::array<Knot,7> knots(const Route& r)
{
    std::array<Knot,7> out{};
    out[0]={0,double(r.start),0};
    const auto leg=[&](int i,long to,double speed,double acceleration) {
        const auto b=out[i];
        const double sign=to>b.x ? 1 : -1;
        const double ramp=speed*1000/acceleration;
        const double cruise=std::abs(to-b.x)/speed-ramp;
        require(cruise>0,"Commanded plateau is infeasible");
        out[i+1]={b.t+ramp,b.x+sign*speed*ramp/2,sign*speed};
        out[i+2]={b.t+ramp+cruise,to-sign*speed*ramp/2,sign*speed};
        out[i+3]={b.t+2*ramp+cruise,double(to),0};
    };
    leg(0,r.apex,r.v1,r.a1);
    leg(3,r.end,r.v2,r.a2);
    return out;
}

template<class Read>
void execute(const Route& r,const Limits& l,Recorder& card,Read read,std::ostream& log,bool& active)
{
    const auto points=knots(r);
    std::array<double,7> t{},x{},v{};
    log<<std::setprecision(17);
    for (std::size_t i=0;i<points.size();++i) {
        t[i]=points[i].t;
        x[i]=points[i].x;
        v[i]=points[i].v;
        log<<"knot_"<<i<<'='<<t[i]<<','<<x[i]<<','<<v[i]<<'\n';
    }
    card.check("GTN_PrfPvt",GTN_PrfPvt(core,axis));
    read(true);
    card.check("GTN_PvtTable",GTN_PvtTable(core,l.table,static_cast<long>(t.size()),t.data(),x.data(),v.data()));
    card.check("GTN_PvtTableSelect",GTN_PvtTableSelect(core,axis,l.table));
    card.check("GTN_SetPvtLoop",GTN_SetPvtLoop(core,axis,1));
    long count=0,loop=0;
    card.check("GTN_GetPvtLoop",GTN_GetPvtLoop(core,axis,&count,&loop));
    log<<"pvt_loop="<<loop<<'\n';
    require(loop==1,"PVT execution is not configured as one shot");
    const auto initial=read(true);
    require(initial.idle() && std::abs(initial.planX-r.start)<=1 && std::abs(initial.x-r.start)<=2,
            "PVT start differs from first knot");
    require(!stopRequested(),"Operator stop");
    event(card,log,"start_before");
    active=true;
    card.check("GTN_PvtStart",GTN_PvtStart(core,axisMask));
    event(card,log,"start_after");
    const auto deadline=Clock::now()+std::chrono::duration<double,std::milli>(t.back()+1800);
    bool moved=false,turned=false;
    while (Clock::now()<deadline) {
        require(!stopRequested(),"Operator stop");
        const auto s=read(true);
        short table=0;
        double elapsed=0;
        card.check("GTN_PvtStatus",GTN_PvtStatus(core,axis,&table,&elapsed));
        require(table==l.table && std::isfinite(elapsed),"Invalid PVT status");
        moved |= bool(s.status&movingMask) && std::abs(s.x-r.start)>15;
        turned |= std::abs(s.x-r.apex)<=100 || elapsed>=t[3];
        log<<"pvt_state="<<Axis::hostUs(Clock::now())<<','<<table<<','<<elapsed<<','<<s.x<<'\n';
        if (moved && turned && s.atTailPosition(r.end) && std::abs(s.planV)<=velocityTol) {
            active=false;
            event(card,log,"arrival");
            return;
        }
        std::this_thread::sleep_for(5ms);
    }
    throw std::runtime_error("PVT completion timeout");
}
int capture(const Route& r,const fs::path& output,const Limits& l)
{
    require(l.lo>=0 && l.hi<=37000 && l.follow>=1 && l.follow<=500 &&
            l.reserve>=1 && l.stop>0 && l.response>=0 && l.response<=1000 &&
            l.table>=1 && l.table<=32,"Invalid site limits or PVT table");
    const double need=l.reserve+l.follow+10+50*l.response+2500/(2*l.stop);
    const auto low=std::min({r.start,r.apex,r.end});
    const auto high=std::max({r.start,r.apex,r.end});
    require(low-l.lo>=need && l.hi-high>=need,"PVT stopping clearance failed");
    require(fs::is_directory(output) && !fs::exists(output/"native.txt"),"Invalid or reused attempt");
    require(readFields(output/"pre.txt").at("ready")=="1","Independent precheck missing");
    require(line("Type START to execute this engineering probe: ")=="START","Cancelled");
    require(SetConsoleCtrlHandler(onCtrl,TRUE)!=FALSE,"Stop handler unavailable");
    auto log=createFile(output/"native.txt");
    auto raw=createFile(output/"watch.csv");
    auto guardLog=createFile(output/"sdk_velocity.csv");
    auto batches=createFile(output/"watch_batches.csv");
    raw<<csvHeader<<'\n';
    guardLog<<"host_us,encoder_card_ms,position_pulse,planner_mm_s,estimated_abs_mm_s,following_error_pulse,status,host_begin_us,host_end_us,sdk_velocity_pulse_ms,velocity_card_ms,rc_velocity\n";
    batches<<"column,type,host_begin_us,host_end_us,rc,capacity,count\n";
    log<<"route="<<r.id<<"\ntable="<<l.table<<"\nfirst_pulse="<<r.start<<"\napex_pulse="<<r.apex
       <<"\nlast_pulse="<<r.end<<"\n";
    Recorder card(log);
    Axis motor(card,log,[](const State&){});
    Guard guard(l,guardLog);
    double follow=l.follow;
    const auto read=[&](bool pvt) {
        const auto s=motor.feedback();
        require((s.status&enableMask) && !(s.status&faultMask) && !s.plus && !s.minus && s.mode==2 &&
                s.planner==(pvt?PROFILE_MODE_PVT:PROFILE_MODE_TRAP),"Axis status, limits or planner invalid");
        require(std::isfinite(s.x) && std::isfinite(s.planX) && std::isfinite(s.planV) && std::isfinite(s.error)
                && s.x>=low-10. && s.x<=high+10. && std::abs(s.error)<=follow
                && std::abs(s.x-s.planX)<=follow,"Axis position or following error invalid");
        guard.check(s);
        return s;
    };
    bool started=false,active=false,complete=false,restored=false;
    auto origin=Clock::now();
    Tick first=0;
    try {
        card.connect();
        require(verifyCorePeriod(card,log)==50000,"Watch requires 50 us");
        double smooth=0,abrupt=0;
        card.check("GTN_GetStopDec",GTN_GetStopDec(core,axis,&smooth,&abrupt));
        log<<"smooth_stop_dec_pulse_ms2="<<smooth<<'\n';
        require(std::isfinite(smooth) && smooth>=l.stop && std::isfinite(abrupt) && abrupt>0,"Stop deceleration invalid");
        long hardwareFollow=0;
        card.check("GTN_GetAxisPosErrLimit",GTN_GetAxisPosErrLimit(core,axis,&hardwareFollow));
        require(hardwareFollow>=0,"Invalid hardware following limit");
        if (hardwareFollow) follow=std::min(follow,double(hardwareFollow));
        const auto initial=read(false);
        unsigned long now=0;
        card.check("GTN_GetClock",GTN_GetClock(core,&now));
        require(now-initial.encoderClock<=50 && initial.idle() && std::abs(initial.x-r.start)<=2 &&
                std::abs(initial.planX-r.start)<=1,"Prepare the first knot and verify encoder time");
        card.configure();
        first=event(card,log,"watch_before");
        origin=Clock::now();
        card.start();
        started=true;
        std::this_thread::sleep_for(10ms);
        const auto watch=card.info("started");
        require(watch.run && watch.mode==WATCH_MODE_STATIC && watch.interval==interval
                && watch.varCount==channels.size(),"Watch configuration mismatch");
        hold([&]{return read(false);},card,log,r.start,1000,"initial_hold");
        execute(r,l,card,read,log,active);
        hold([&]{return read(true);},card,log,r.end,r.tail,"terminal_hold");
        card.check("GTN_PrfTrap",GTN_PrfTrap(core,axis));
        restored=read(false).atTailPosition(r.end);
        require(restored,"TRAP restore readback failed");
        complete=true;
    } catch (const std::exception& e) {
        log<<"reason="<<e.what()<<'\n';
        if (active) {
            const short rc=GTN_Stop(core,axisMask,0);
            log<<"smooth_stop_rc="<<rc<<'\n';
            bool stopped=false;
            const auto deadline=Clock::now()+2s;
            while (Clock::now()<deadline) {
                long status=0;
                double v=0;
                if (!GTN_GetSts(core,axis,&status) && !GTN_GetAxisEncVel(core,axis,&v)
                    && !(status&movingMask) && std::abs(v)<=velocityTol) {
                    stopped=true;
                    break;
                }
                std::this_thread::sleep_for(10ms);
            }
            log<<"stop_observed="<<stopped<<'\n';
        }
    }
    bool aligned=false;
    if (started) {
        try {
            const auto last=event(card,log,"watch_end_request");
            aligned=saveWatch(card,log,batches,raw,origin,300000,first,last+2000);
        } catch (const std::exception& e) { log<<"watch_error="<<e.what()<<'\n'; }
    }
    card.cleanup();
    complete=complete && restored && aligned && card.cleanupOk;
    log<<"watch_contiguous="<<aligned<<"\ntrap_restored="<<restored<<"\noutcome="
       <<(complete?"complete":"incomplete")<<'\n';
    return complete?0:1;
}
}

int wmain(int argc,wchar_t** argv)
{
    try {
        if (argc==2 && std::wstring(argv[1])==L"--state") return readStatus();
        if (argc>=3 && (std::wstring(argv[1])==L"--plan" || std::wstring(argv[1])==L"--capture")) {
            const auto name=fs::path(argv[2]).string();
            const auto it=std::find_if(routes.begin(),routes.end(),[&](const Route& r){return r.id==name;});
            require(it!=routes.end(),"Unknown turn route");
            if (argc==3 && std::wstring(argv[1])==L"--plan") {
                std::cout<<"time_ms,position_pulse,velocity_pulse_ms\n"<<std::setprecision(17);
                for (const auto& k:knots(*it)) std::cout<<k.t<<','<<k.x<<','<<k.v<<'\n';
                return 0;
            }
            require(argc==4 && std::wstring(argv[1])==L"--capture","Invalid arguments");
            const fs::path path(argv[3]);
            std::ifstream in(path/"limits.txt");
            Limits limits{};
            require(bool(in>>limits.lo>>limits.hi>>limits.reserve>>limits.follow>>limits.stop>>limits.response>>limits.table),
                    "Missing site limits");
            return capture(*it,path,limits);
        }
        std::cout<<"Use turn_probe.py for preview, self-test, or authorized capture.\n";
        return argc==1?0:1;
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<'\n';
        return 1;
    }
}
