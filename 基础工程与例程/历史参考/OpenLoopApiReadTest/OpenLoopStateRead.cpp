// READ ONLY: no NetInit, reset, enable, motion, target write or mode write.
// Fixed CORE=2 AXIS=1 INDEX=1 channel=5. Failed reads never mean zero state.
#include <cstdio>
#include <cmath>
#include "gvn_force_api_ext.h"
int main() {
 SetConsoleOutputCP(CP_UTF8);
 const short core=2,axis=1,index=1;
 SYSTEMTIME t;GetLocalTime(&t);char name[160];
 std::snprintf(name,sizeof(name),"state_read_%04u%02u%02u_%02u%02u%02u_%03u.log",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds);
 FILE* f=nullptr;if(fopen_s(&f,name,"w"))return 2;
 #define STATE_LOG(...) do{std::printf(__VA_ARGS__);std::fprintf(f,__VA_ARGS__);std::fflush(f);}while(0)
 char path[32768]={};GetModuleFileNameA(GetModuleHandleA("gts.dll"),path,32768);
 STATE_LOG("READ ONLY. No network reinitialization or control writes.\nDLL=%s\n",path);
 short rc=GTN_OpenCard(5);STATE_LOG("OpenCard(5) rc=%d\n",rc);
 if(rc){STATE_LOG("CURRENT STATE UNKNOWN: open failed.\n");std::fclose(f);return 1;}
 bool all=true,ready=true;short lastMode=-1;double lastTarget=0;long lastSts=0;
 for(int i=0;i<10;++i){
  long sts=0;short mode=-1,current=0;double target=0,enc=0,prf=0,vel=0;
  TSwitchLoopParameter sw={};
  short a=GTN_GetSts(core,axis,&sts);
  short b=GTN_GetControlLoopMode(core,axis,index,&mode);
  short c=GTN_GetForceValue(core,axis,index,&target);
  short d=GTN_GetSwitchLoopParameter(core,axis,&sw);
  short e=GTN_GetAxisEncPos(core,axis,&enc);
  short g=GTN_GetPrfPos(core,axis,&prf);
  short h=GTN_GetAxisEncVel(core,axis,&vel);
  short j=GTN_GetAtlTorque(core,axis,&current);
  const bool primary=!a&&!b&&!c&&!d&&std::isfinite(target);
  const bool valid=primary&&!e&&!g&&!h&&!j&&std::isfinite(enc)&&std::isfinite(prf)&&std::isfinite(vel);
  STATE_LOG("sample=%d rc[sts,mode,target,switch,enc,prf,vel,current]=[%d,%d,%d,%d,%d,%d,%d,%d] valid=%s\n",i,a,b,c,d,e,g,h,j,valid?"YES":"NO");
  if(primary){
   STATE_LOG("status=0x%08lX enabled=%d mode=%d target=%.17g switchEnable=%d switchMode=%d\n",sts,(sts&0x200)?1:0,mode,target,sw.enable,sw.mode);
   lastSts=sts;lastMode=mode;lastTarget=target;
   ready=ready&&!(sts&(0x2|0x10|0x20|0x40|0x100|0x200|0x400))&&mode==2&&target==0&&sw.enable==0;
  } else {all=false;ready=false;STATE_LOG("Primary state UNKNOWN: read failure; no control action taken.\n");}
  if(valid)STATE_LOG("enc=%.6f prf=%.6f vel=%.6f current_raw=%d\n",enc,prf,vel,current);
  else {ready=false;STATE_LOG("Additional telemetry incomplete; no network reinitialization attempted.\n");}
  Sleep(100);
 }
 short close=GTN_Close();STATE_LOG("Close rc=%d\n",close);
 if(all)STATE_LOG("LAST VALID: enabled=%d mode=%d target=%.17g\n",(lastSts&0x200)?1:0,lastMode,lastTarget);
 STATE_LOG("READ=%s BASE_STATE=%s (not a physical safety certification).\nLog=%s\n",all?"PASS":"INCOMPLETE",ready?"DISABLED_MODE2_ZERO_NO_PENDING":"NOT CONFIRMED",name);
 std::fclose(f);return all&&!close?0:1;
}
