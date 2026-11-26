#include <conio.h>
#include <windows.h>
#include <stdio.h>
#include "gxn.h"

#define AXIS 1
#define CORE 2

void commandhandler(char* command, short error)
{
    // 如果指令执行返回值为非0，说明指令执行错误，向屏幕输出错误结果
   
        printf("%s = %d\n", command, error);
    
}

int main(int argc, char* argv[])
{
    short sRtn;
    TTrapPrm trap;
    long pSts;
    double prfPos;

    // 打开运动控制器
    sRtn = GTN_OpenCard();
    // 指令返回值检测，请查阅例3-1
    commandhandler(const_cast<char*>("GTN_OpenCard"), sRtn);


  sRtn = GTN_NetInit(NET_INIT_MODE_XML_STRICT,const_cast<char*>("C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml"),60, &pSts);
  commandhandler(const_cast<char*>("GTN_NetInit"), sRtn);
    // 清除各轴的报警和限位
    sRtn = GTN_ClrSts(CORE,AXIS,1);
    commandhandler(const_cast<char*>("GTN_ClrSts"), sRtn);

    // 伺服使能
    sRtn = GTN_AxisOn(CORE, AXIS);
    commandhandler(const_cast<char*>("GTN_AxisOn"), sRtn);

    // 位置清零
    sRtn = GTN_ZeroPos(CORE, AXIS);
    commandhandler(const_cast<char*>("GTN_ZeroPos"), sRtn);

    // AXIS轴规划位置清零
    sRtn = GTN_SetPrfPos(CORE,1,0);
    commandhandler(const_cast<char*>("GTN_SetPrfPos"), sRtn);

    // 将 AXIS 轴设为点位模式
    sRtn = GTN_PrfTrap(2,1);
    commandhandler(const_cast<char*>("GTN_PrfTrap"), sRtn);

    // 读取点位运动参数(需要读取所有运动参数到上位机变量)
    sRtn = GTN_GetTrapPrm(CORE,1, &trap);
    commandhandler(const_cast<char*>("GTN_GetTrapPrm"), sRtn);

    // 设置需要修改的运动参数
    trap.acc = 0.25;
    trap.dec = 0.125;
    trap.smoothTime = 25;

    // 设置点位运动参数
    // 正确调用方式
    sRtn = GTN_SetTrapPrm(CORE, 1, &trap); // profile 参数设为 0

    commandhandler(const_cast<char*>("GNT_SetTrapPrm"), sRtn); // 注意这里应该是"GNT_SetTrapPrm"还是"GTN_SetTrapPrm"

    // 设置 AXIS 轴的目标位置
    sRtn = GTN_SetPos(2,1,131072);
    commandhandler(const_cast<char*>("GT_SetPos"), sRtn); // 注意这里应该是"GT_SetPos"还是"GTN_SetPos"

    // 设置AXIS轴的目标速度
    sRtn = GTN_SetVel(2, 1,50);
    commandhandler(const_cast<char*>("GT_SetVel"), sRtn); // 注意这里应该是"GT_SetVel"还是"GTN_SetVel"

    // 启动AXIS轴的运动
    sRtn = GTN_Update(CORE,1 << (AXIS - 1));
    commandhandler(const_cast<char*>("GT_Update"), sRtn); // 注意这里应该是"GT_Update"还是"GTN_Update"

    do
    {
        // 读取AXIS轴的状态
        sRtn = GTN_GetSts(CORE, AXIS, &pSts);

        // 读取AXIS轴的规划位置
        sRtn = GTN_GetPrfPos(CORE, AXIS, &prfPos);
        printf("sts=0x%-10lx  prfPos=%-10.1lf\r", pSts, prfPos);

    } while (pSts & 0x400); // 等待AXIS轴规划停止

    // 伺服关闭
    sRtn = GTN_AxisOff(CORE, AXIS);
    printf("\nGT_AxisOff()=%d\n", sRtn);

    int ch = _getch(); // 捕获返回值
    printf("您按下了字符: %c\n", ch);
    return 0;
}