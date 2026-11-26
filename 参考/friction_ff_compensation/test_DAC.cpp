// MotionPrfTorque.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include "stdafx.h"
#include "conio.h"
#include "gxn.h"

#include "string.h"

//加载gxn.dll库

/*接GSHD驱动器，驱动器控制模式设置为力矩模式，通过设置力矩指令来控制电机运动，控制卡规划位置不发生变化，当调用GTN_SetPrfTorque并
设置了力矩后，目标轴会一致运动，只有再次将力矩参数设置为0并再次调用设置力矩指令后才会停止，在这个过程中
GTN_Stop指令无效，用户使用过程中，有撞机危险请谨慎使用*/

//定义指令返回值判断函数
void CommandHandler(char* command, short error)
{
	if (error)
	{
		printf("%s = %d\n", command, error);
	}
}

int _tmain(int argc, _TCHAR* argv[])
{
	short rtn;
	short core = 1;
	short axis = 1;
	short motionMode;
	short mode = 0;
	short time;
	long sts;


	//打开控制器
	rtn = GTN_OpenCard(5);
	CommandHandler(const_cast < char*>("GTN_OpenCard"), rtn);

	//网络初始化
	rtn = GTN_NetInit(NET_INIT_MODE_XML_STRICT, const_cast<char*>("C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml"), 60, &sts);
	CommandHandler(const_cast < char*>("GTN_NetInit"), rtn);

	//复位运动控制器
	rtn = GTN_Reset(core);
	CommandHandler(const_cast < char*>("GTN_Reset"), rtn);

	//取消轴报警
	rtn = GTN_AlarmOff(core, axis);
	CommandHandler(const_cast < char*>("GTN_AlarmOff"), rtn);

	//取消轴限位
	rtn = GTN_LmtsOff(core, axis);
	CommandHandler(const_cast < char*>("GTN_LmtsOff"), rtn);

	//目标轴下使能
	rtn = GTN_AxisOff(core, axis);
	CommandHandler(const_cast < char*>("GTN_AxisOff"), rtn);

	//清除轴状态
	rtn = GTN_ClrSts(core, axis);
	CommandHandler(const_cast < char*>("GTN_ClrSts"), rtn);

	//清除位置信息
	rtn = GTN_ZeroPos(core, axis);
	CommandHandler(const_cast < char*>("GTN_ZeroPos"), rtn);


	//设置控制卡环路模式
	rtn = GTN_CtrlMode(core, axis, mode);//mode=0，控制卡上轴设置为位置环，mode=1控制卡设置为脉冲模式
	CommandHandler(const_cast < char*>("GTN_CtrlMode"), rtn);

	//读取驱动器环路模式
	rtn = GTN_GetMotionMode(core, axis, &motionMode);
	CommandHandler(const_cast < char*>("GTN_GetMotionMode"), rtn);

	motionMode = 7;
	//设置驱动器环路模式
	rtn = GTN_SetMotionMode(core, axis, motionMode);// motionMode= 6 驱动器设置为周期同步速度模式, motionMode = 5 驱动器设置为周期同步位置模式；当motionMode= 6时mode必须为，当motionMode=5时，mode必须为。
	CommandHandler(const_cast < char*>("GTN_SetMotionMode"), rtn);

	rtn = GTN_GetMotionMode(core, axis, &motionMode);
	CommandHandler(const_cast < char*>("GTN_GetMotionMode"), rtn);

	double prfTorqueNM = 0.02;   // 力矩N（N/m）
	double current2Torque = 0.4;  // 力矩系数，驱动器电机参数
	double ratedCurrent = 2.5;  // 额定电流，驱动器电机参数
	short prfTorque;                 //输入参数
	short pAtlTorque;                //读取输入的参数
	prfTorque = 1000 * prfTorqueNM / current2Torque / ratedCurrent;
	//目标轴上伺服使能

	rtn = GTN_AxisOn(1, axis);
	CommandHandler(const_cast < char*>("GTN_AxisOn"), rtn);

	//设置目标力矩。输入参数需经过转换公式计算
	rtn = GTN_SetPrfTorque(core, axis, prfTorque);
	CommandHandler(const_cast < char*>("GTN_SetPrfTorque"), rtn);

	rtn = GTN_GetAtlTorque(core, axis, &pAtlTorque);
	CommandHandler(const_cast < char*>("GTN_GetAtlTorque"), rtn);

	//力矩模式无法读取规划位置，因此，循环读取10000次轴状态后停止
	for (time = 0;time < 10000;time++)
	{
		rtn = GTN_GetSts(core, axis, &sts);
		CommandHandler(const_cast < char*>("GTN_GetSts"), rtn);
	}

	prfTorque = 0;
	//设置目标力矩（prfTorque设为0时即表示目标轴停止运动）
	rtn = GTN_SetPrfTorque(core, axis, prfTorque);

	return 0;
}

