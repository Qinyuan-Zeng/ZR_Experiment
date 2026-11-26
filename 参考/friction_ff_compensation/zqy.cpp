// MoveVelocity.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"
#include "conio.h"
#include "gxn.h"
#include "string.h"


/*测试功能，MoveVelocity从当前速度按照设定的加速度和加加速度运动到目标速度，
到达目标速度以后一直保持目标速度。*/

void CommandHandler(char* command, short error)
{
	printf("%s = %d\n", command, error);
}
int _tmain(int argc, _TCHAR* argv[])
{
	short rtn;
	short core = 2;
	short axis = 1;
	short limitType = -1;
	short count = 1;
	TJogPrm jogPrm;
	long sts;
	double prfPos, prfVel;
	TMoveVelocityPrm moveVelocityPrm;

	//打开控制器
	rtn = GTN_OpenCard();
	CommandHandler(const_cast<char*>("GTN_OpenCard"), rtn);

	//网络初始化
	rtn = GTN_NetInit(NET_INIT_MODE_XML_STRICT, const_cast<char*>("C:\\Users\\MCLab\\Desktop\\MotionStudio_x64_V2.3.14.0_develop\\Googol.xml"), 60, &sts);
	CommandHandler(const_cast<char*>("GTN_NetInit"), rtn);


	// 清除各轴的报警和限位
	rtn = GTN_LmtsOff(core, axis, limitType);
	CommandHandler(const_cast<char*>("GTN_LmtsOff"), rtn);

	rtn = GTN_AlarmOff(core, axis);
	CommandHandler(const_cast<char*>("GTN_AlarmOff"), rtn);


	rtn = GTN_ClrSts(core, axis, count);
	CommandHandler(const_cast<char*>("GTN_ClrSts"), rtn);

	// 伺服使能
	rtn = GTN_AxisOn(core, axis);
	CommandHandler(const_cast<char*>("GTN_AxisOn"), rtn);

	// 位置清零
	rtn = GTN_ZeroPos(core, axis);
	CommandHandler(const_cast<char*>("GTN_ZeroPos"), rtn);

	// 将AXIS轴设为Jog模式
	rtn = GTN_PrfJog(core, axis);
	CommandHandler(const_cast<char*>("GTN_PrfJog"), rtn);

	// 读取Jog运动参数(需要读取全部运动参数到上位机变量)
	rtn = GTN_GetJogPrm(core, axis, &jogPrm);
	CommandHandler(const_cast<char*>("GTN_GetJogPrm"), rtn);

	//设置需要修改的运动参数
	memset(&jogPrm, 0, sizeof(jogPrm));
	jogPrm.acc = 0.0625;
	jogPrm.dec = 0.0625;

	// 设置Jog运动参数
	rtn = GTN_SetJogPrm(core, axis, &jogPrm);
	CommandHandler(const_cast<char*>("GTN_SetJogPrm"), rtn);

	// 设置AXIS轴的初始速度
	rtn = GTN_SetVel(core, axis, 100);
	CommandHandler(const_cast<char*>("GTN_SetVel"), rtn);

	// 启动AXIS轴的运动
	rtn = GTN_Update(core, 1 << (axis - 1));
	CommandHandler(const_cast<char*>("GTN_Update"), rtn);


	while (1)
	{
		// 读取AXIS轴的状态
		rtn = GTN_GetSts(core, axis, &sts);
		CommandHandler(const_cast<char*>("GTN_GetSts"), rtn);

		// 读取AXIS轴的规划位置
		rtn = GTN_GetPrfPos(core, axis, &prfPos);
		CommandHandler(const_cast<char*>("GTN_GetPrfPos"), rtn);

		// 读取AXIS轴的规划速度
		rtn = GTN_GetPrfVel(core, axis, &prfVel);
		CommandHandler(const_cast<char*>("GTN_GetPrfVel"), rtn);

		printf(const_cast<char*>("sts=0x%-10lxprfPos=%-10.1lfprfVel=%-10.1lf\r"), sts, prfPos, prfVel);
		if (prfPos >= 1000000)
		{
			moveVelocityPrm.acc = 0.5;
			moveVelocityPrm.dec = 0.25;
			moveVelocityPrm.vel = 200;
			moveVelocityPrm.jerkBegin = 5;
			moveVelocityPrm.direction = 0;

			rtn = GTN_MoveVelocity(core, axis, &moveVelocityPrm);
			CommandHandler(const_cast<char*>("GTN_MoveVelocity"), rtn);

			// 读取AXIS轴的规划位置
			rtn = GTN_GetPrfPos(core, axis, &prfPos);
			CommandHandler(const_cast<char*>("GTN_GetPrfPos"), rtn);

			// 读取AXIS轴的规划速度
			rtn = GTN_GetPrfVel(core, axis, &prfVel);
			CommandHandler(const_cast<char*>("GTN_GetPrfVel"), rtn);

			printf(const_cast<char*>("sts=0x%-10lxprfPos=%-10.1lfprfVel=%-10.1lf\r"), sts, prfPos, prfVel);
			if (prfPos > 2000000)
			{
				break;
			}
		}
	}

	// 伺服关闭
	rtn = GTN_AxisOff(core, axis);
	CommandHandler(const_cast<char*>("GTN_AxisOff"), rtn);

	//关闭控制器
	rtn = GTN_Close();
	CommandHandler(const_cast<char*>("GTN_Close"), rtn);

	return 0;
}