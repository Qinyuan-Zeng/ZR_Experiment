// PVT_Pvt.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include "gxn.h"
#include "conio.h"
#include "string.h"



// 函数定义保持不变，参数为 char* (非常量)
void CommandHandler(char* command, short error)
{
    
        printf("%s = %d\n", command, error);

}

//主函数
int _tmain(int argc, _TCHAR* argv[])
{
    short rtn;
    short mask;
    short core = 2;
    short axis = 1;
    short TABLE = 1;
    double time[6] = { 0, 1200, 2000, 3000, 3800, 5000 };
    double pos[6] = { 0, 9750, 24483, 44483, 59216, 68966 };
    double vel[6] = { 1, 15.25, 20, 20, 15.25, 1 };
    double prfVel, prfPos, time1;
    short tableId;
    short count = 1;
    long sts;
  

    //打开控制器
    rtn = GTN_OpenCard();
    CommandHandler(const_cast<char*>("GTN_OpenCard"), rtn);

    //网络初始化
    rtn = GTN_NetInit(NET_INIT_MODE_XML_STRICT, const_cast<char*>("C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml"), 60, &sts);
    CommandHandler(const_cast<char*>("GTN_NetInit"), rtn);

    printf("%s = %d\n","sts", sts);
    //清除状态
    rtn = GTN_ClrSts(core, axis, count);
    CommandHandler(const_cast<char*>("GTN_ClrSts"), rtn);

    // 伺服使能
    rtn = GTN_AxisOn(core, axis);
    CommandHandler(const_cast<char*>("GTN_AxisOn"), rtn);

    // 位置清零
    rtn = GTN_ZeroPos(core, axis);
    CommandHandler(const_cast<char*>("GTN_ZeroPos"), rtn);

    //等待伺服使能就绪
    Sleep(1000);

    // 设置为PVT模式
    rtn = GTN_PrfPvt(core, axis);
    CommandHandler(const_cast<char*>("GTN_PrfPvt"), rtn);

    // AXIS轴规划位置清零
    rtn = GTN_SetPrfPos(core, 1, 0);
    CommandHandler(const_cast<char*>("GTN_SetPrfPos"), rtn);

    // 发送数据
    rtn = GTN_PvtTable(core, TABLE, 6, &time[0], &pos[0], &vel[0]);
    CommandHandler(const_cast<char*>("GTN_PvtTable"), rtn);

    // 选择数据表
    rtn = GTN_PvtTableSelect(core, axis, TABLE);
    CommandHandler(const_cast<char*>("GTN_PvtTableSelect"), rtn);

    mask = 1 << (axis - 1);
    rtn = GTN_PvtStart(core, mask);
    CommandHandler(const_cast<char*>("GTN_PvtStart"), rtn);

    Sleep(1000);
    do
    {
        rtn = GTN_GetSts(core, axis, &sts);
        CommandHandler(const_cast<char*>("GTN_GetSts"), rtn);
        printf(" %d\n", sts);
//
        // 读取数据表和运动时间
        rtn = GTN_PvtStatus(core, axis, &tableId, &time1);
        CommandHandler(const_cast<char*>("GTN_PvtStatus"), rtn);

        // 读取规划速度
        rtn = GTN_GetPrfVel(core, axis, &prfVel);
        CommandHandler(const_cast<char*>("GTN_GetPrfVel"), rtn);

        // 读取规划位置
        rtn = GTN_GetPrfPos(core, axis, &prfPos);
        CommandHandler(const_cast<char*>("GTN_GetPrfPos"), rtn);
        

        printf("数据表ID号=%2d 轴运动时间 = %10.0lf 轴运动速度 =%10.2lf 轴运动位置 = %10.1lf\r", tableId, time1, prfVel, prfPos);

    } while (sts & 0x400);


    // 伺服下使能
    rtn = GTN_AxisOff(core, axis);
    CommandHandler(const_cast<char*>("GTN_AxisOff"), rtn);

    //关闭控制器
    rtn = GTN_Close();
    CommandHandler(const_cast<char*>("GTN_Close"), rtn);

    return 0;
}