
// FcorceControl_TestDlg.cpp : 实现文件
//

#include "stdafx.h"
#include "FcorceControl_Test.h"
#include "FcorceControl_TestDlg.h"
#include "afxdialogex.h"
#include <afx.h>
#include "windows.h"

#include"gxn.h"
//加载gxn.dll库
#pragma comment (lib,"gxn.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

//全局变量定义
short gRtn;
short gCore = 2;
short gAxis = 1;
short gAdcIndex;
short gGlobalErrCount = 0;
char* gStage = "等待运动";//表示当前运动的阶段;
double gVelFastMove;
double gVelSearchMove;
double gPosFastMoveEnd;
double gPosSearchEnd;
double gPosBack;
double gVelBack;
double gFeedbackForceK;
double gFeedbackForceB;
long gPosToForceSwitchDir;
long gPosToForceSwitchThreshold;
double gPosToForceSwitch0verTime;
double gForceTarget1;
double gForceTarget2;
double gForceTarget3;
double gTimeForceSustain;

CFcorceControl_TestDlg::CFcorceControl_TestDlg(CWnd* pParent /*=NULL*/)
	: CDialogEx(CFcorceControl_TestDlg::IDD, pParent)
	, axis(0)
	, posFastMoveEnd(0)
	, posSearchEnd(0)
	, forceTarget1(0)
	, timeForceSustain(0)
	, feedbackForceK(0)
	, feedbackForceB(0)
	, adcIndex(0)
	, velFastMove(0)
	, velSearchMove(0)
	, controlforceK(0)
	, controlforceB(0)
	, forceProtect(0)
	, forceLimit(0)
	, forceKp(0)
	, forceKi(0)
	, forceKd(0)
	, forceTarget2(0)
	, forceTarget3(0)
	, posBack(0)
	, velBack(0)
	, posToForceSwitchDir(0)
	, posToForceSwitchThreshold(0)
	, posToForceSwitch0verTime(0)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CFcorceControl_TestDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Text(pDX, IDC_EDIT1, axis);	
	gAxis = axis;
	DDX_Text(pDX, IDC_EDIT9, velFastMove);
	gVelFastMove = velFastMove;
	DDX_Text(pDX, IDC_EDIT10, velSearchMove);
	gVelSearchMove = velSearchMove;
	DDX_Text(pDX, IDC_EDIT2, posFastMoveEnd);
	gPosFastMoveEnd = posFastMoveEnd;
	DDX_Text(pDX, IDC_EDIT3, posSearchEnd);
	gPosSearchEnd = posSearchEnd;
	DDX_Text(pDX, IDC_EDIT4, forceTarget1);
	gForceTarget1 = forceTarget1;
	DDX_Text(pDX, IDC_EDIT18, forceTarget2);
	gForceTarget2 = forceTarget2;
	DDX_Text(pDX, IDC_EDIT19, forceTarget3);
	gForceTarget3 = forceTarget3;
	DDX_Text(pDX, IDC_EDIT5, timeForceSustain);
	gTimeForceSustain = timeForceSustain;
	DDX_Text(pDX, IDC_EDIT_PosBack, posBack);
	gPosBack = posBack;
	DDX_Text(pDX, IDC_EDIT_VelBack, velBack);
	gVelBack = velBack;
	DDX_Text(pDX, IDC_EDIT_PosToForceSwitchDir, posToForceSwitchDir);
	gPosToForceSwitchDir = posToForceSwitchDir;
	DDX_Text(pDX, IDC_EDIT_PosToForceSwitchThreshold, posToForceSwitchThreshold);
	gPosToForceSwitchThreshold = posToForceSwitchThreshold;
	DDX_Text(pDX, IDC_EDIT_PosToForceSwitch0verTime, posToForceSwitch0verTime);
	gPosToForceSwitch0verTime = posToForceSwitch0verTime;
}

BEGIN_MESSAGE_MAP(CFcorceControl_TestDlg, CDialogEx)
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_WM_TIMER()
	ON_BN_CLICKED(IDC_BUTTON_MoveFroceControl, &CFcorceControl_TestDlg::OnBnClickedButtonMovefrocecontrol)
	ON_BN_CLICKED(IDC_BUTTON_OpenCard, &CFcorceControl_TestDlg::OnBnClickedButtonOpencard)
	ON_BN_CLICKED(IDC_BUTTON_CalculateForceAvg, &CFcorceControl_TestDlg::OnBnClickedButtonCalculateforceavg)
END_MESSAGE_MAP()


// 如果向对话框添加最小化按钮，则需要下面的代码
//  来绘制该图标。对于使用文档/视图模型的 MFC 应用程序，
//  这将由框架自动完成。

void CFcorceControl_TestDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // 用于绘制的设备上下文

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// 使图标在工作区矩形中居中
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// 绘制图标
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

//当用户拖动最小化窗口时系统调用此函数取得光标
//显示。
HCURSOR CFcorceControl_TestDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

void CFcorceControl_TestDlg::CommandHandler(char* command, short error)
{
	CString strTemp;
	if (error)
	{
		gGlobalErrCount++;//记录例程中指令报错总数
		strTemp.Format(_T("%s = %d\n commandErrCount=%d"),command,error,gGlobalErrCount);
		GetDlgItem(IDC_STATIC_command)->SetWindowText(strTemp);
	}
}

void CFcorceControl_TestDlg::OnBnClickedButtonOpencard()
{
	//设置预设参数
	GetDlgItem(IDC_EDIT1)->SetWindowText("1");//轴号
	GetDlgItem(IDC_EDIT2)->SetWindowText("2000.00");//快进终点
	GetDlgItem(IDC_EDIT9)->SetWindowText("5");//快进速度
	GetDlgItem(IDC_EDIT3)->SetWindowText("4000.00");//搜索终点
	GetDlgItem(IDC_EDIT10)->SetWindowText("2");//搜索速度
	GetDlgItem(IDC_EDIT_PosBack)->SetWindowText("0.00");//回退终点
	GetDlgItem(IDC_EDIT_VelBack)->SetWindowText("5");//回退终点
	GetDlgItem(IDC_EDIT_PosToForceSwitchDir)->SetWindowText("1");//运控切力控时阈值的判断方向
	GetDlgItem(IDC_EDIT_PosToForceSwitchThreshold)->SetWindowText("15");//环路切换的阈值
	GetDlgItem(IDC_EDIT_PosToForceSwitch0verTime)->SetWindowText("1");//环路切换跟随误差超时时间,当跟随误差超过阈值并持续该时间后,进行切换
	GetDlgItem(IDC_EDIT4)->SetWindowText("10");//开环力控dac输出值1
	GetDlgItem(IDC_EDIT18)->SetWindowText("20");//开环力控dac输出值2
	GetDlgItem(IDC_EDIT19)->SetWindowText("30");//开环力控dac输出值3
	GetDlgItem(IDC_EDIT5)->SetWindowText("500");//开环力控间隔时间（ms）
	UpdateData(TRUE);

	short localErrCount = gGlobalErrCount;//localErrCount记录此函数开始时指令报错数量
	GetDlgItem(IDC_STATIC_command)->SetWindowText(_T("开卡中，稍等..."));

	//打开运动控制卡，channel开卡方式默认为5
	gRtn = GTN_OpenCard(5);
	CommandHandler("GTN_OpenCard",gRtn);
	
	SetTimer(1,100,NULL);

	//控制卡网络初始化，gMode网络初始化模式只支持xml严格匹配模式，即模式100
	//指令调用xml需要使用调试软件MotionStudio生成，并放置于程序运行工作目录
	gRtn = GTN_NetInit(100,"Glink.xml");
	CommandHandler("GTN_NetInit",gRtn);

	//gGlobalErrCount记录此函数中指令报错数量，若指令有报错会调用CommandHandler函数，使gGlobalErrCount++；
	//若指令报错无增加，则说明此函数执行正常
	if(localErrCount == gGlobalErrCount)
	{
		GetDlgItem(IDC_STATIC_command)->SetWindowText(_T("开卡完成"));
	}

}

void CFcorceControl_TestDlg::OnTimer(UINT_PTR nIDEvent)
{
	CString strTemp;
	short ctrlMode;
	TPid forcePid;
	short index =1;
	double encPos;
	double prfPos;
	long  axisSts;
	double targetForce;
	double adcValue;
	double actForce;

	gRtn = GTN_GetStsEx(gCore,gAxis,&axisSts);
	CommandHandler("GTN_GetSts",gRtn);

	gRtn =GTN_GetEncPos(gCore,gAxis,&encPos);
	CommandHandler("GTN_GetEncPos",gRtn);

	gRtn =GTN_GetPrfPos(gCore,gAxis,&prfPos);
	CommandHandler("GTN_GetPrfPos",gRtn);

	gRtn = GTN_GetControlLoopMode(gCore, gAxis, index,&ctrlMode);
	CommandHandler("GTN_GetCtrlMode",gRtn);

	gRtn = GTN_GetForceLoopPid(gCore,gAxis,index,&forcePid);
	CommandHandler("GTN_GetForceLoopPid",gRtn);

	gRtn = GTN_GetForceValue(gCore,gAxis,1,&targetForce);
	CommandHandler("GTN_GetForceValue",gRtn);
	
	strTemp.Format("%.3lf",targetForce);
	GetDlgItem(IDC_TargetForce)->SetWindowText(strTemp);
	
	if(axisSts&0x2)
	{
		GetDlgItem(IDC_AxisSts)->SetWindowText("驱动器报警");
	}
	else if(axisSts&0x20||axisSts&0x40)
	{
		GetDlgItem(IDC_AxisSts)->SetWindowText("限位触发");
	}
	else if(axisSts&0x400)
	{
		GetDlgItem(IDC_AxisSts)->SetWindowText("规划运动");
	}
	else if(axisSts&0x200)
	{
		GetDlgItem(IDC_AxisSts)->SetWindowText("使能");
	}
	else if(0x0 == axisSts)
	{
		GetDlgItem(IDC_AxisSts)->SetWindowText("正常");
	}
	else
	{
		GetDlgItem(IDC_AxisSts)->SetWindowText("其它");
	}

	strTemp.Format("%.3lf",prfPos);
	GetDlgItem(IDC_PrfPos)->SetWindowText(strTemp);

	strTemp.Format("%.3lf",encPos);
	GetDlgItem(IDC_EncPos)->SetWindowText(strTemp);

	switch(ctrlMode)
	{
	case 0:
		GetDlgItem(IDC_AXISMODE)->SetWindowText("位置单闭环");
		break;
	case 1:
		GetDlgItem(IDC_AXISMODE)->SetWindowText("开环");
		break;
	case 2:
		GetDlgItem(IDC_AXISMODE)->SetWindowText("位置双闭环");
		break;	
	case 3:
		GetDlgItem(IDC_AXISMODE)->SetWindowText("开环力控");
		break;
	case 4:
		GetDlgItem(IDC_AXISMODE)->SetWindowText("力控双闭环");
		break;
	default:
		GetDlgItem(IDC_AXISMODE)->SetWindowText("未知");
		break;
	}

	GetDlgItem(IDC_Stage)->SetWindowText(gStage);

	CDialogEx::OnTimer(nIDEvent);
}

void CFcorceControl_TestDlg::OnBnClickedButtonMovefrocecontrol()
{
	
	UpdateData(TRUE);
    //// 禁用按钮，防止重复点击
    //GetDlgItem(IDC_BUTTON_MoveFroceControl)->EnableWindow(FALSE);
    //
    //// 准备线程参数
    //ThreadParams* pParams = new ThreadParams;
    //pParams->pDlg = this;
		
    // 启动工作线程

    AfxBeginThread(WorkerThread, this);
}

// 工作线程函数
UINT WorkerThread(LPVOID param) 
{
	/*--------------------------------------------------接触前环路切换参数-----------------------------------------------------*/
	short posToForceSwitchEnable = 1;													// 使能或者禁用环路切换功能,0: 禁用环路切换功能,1: 启用环路切换功能
	short posToForceSwitchmode = 0;													// 切换模式,0: 由运控切换至开环力控,1: 由力控切换至运控；2:由运控切换到双闭环力控
	long posToForceSwitchDir = gPosToForceSwitchDir;						// 运控切力控时阈值的判断方向,0: 小于阈值时进行切换,1: 大于阈值时进行切换
	long posToForceSwitchThreshold = gPosToForceSwitchThreshold;	// 环路切换的阈值,位置环跟随误差超过该阈值时切换环路,单位: 脉冲
	long posToForceSwitchOpenLoopDac = gForceTarget1;					// 从运控切换到力控之后的dac输出值,单位: dac的bit位
	double posToForceSwitch0verTime = gPosToForceSwitch0verTime;	// 环路切换跟随误差超时时间,当跟随误差超过阈值并持续该时间后,进行切换，单位: ms
	long posToForceSwitchPosOffset = 0;												// 力控切换运控时，规划位置相对于编码器位置的偏移量,单位: 脉冲。只有当mode设为1时，此参数才生效。
	/*--------------------------------------------------脱离工件前环路切换参数-----------------------------------------------------*/
	short forceToPosSwitchEnable = 1;                  // 使能或者禁用环路切换功能,0: 禁用环路切换功能,1: 启用环路切换功能
	short forceToPosSwitchmode = 1;                   // 切换模式,0: 由运控切换至开环力控,1: 由力控切换至运控；2:由运控切换到双闭环力控
	long forceToPosSwitchDir = 1;                        // 运控切力控时阈值的判断方向,0: 小于阈值时进行切换,1: 大于阈值时进行切换
	long forceToPosSwitchThreshold = 15;           // 环路切换的阈值,位置环跟随误差超过该阈值时切换环路,单位: 脉冲
	long forceToPosSwitchOpenLoopDac =10;     // 从运控切换到力控之后的dac输出值,单位: dac的bit位
	double forceToPosSwitch0verTime = 1;          // 环路切换跟随误差超时时间,当跟随误差超过阈值并持续该时间后,进行切换，单位: ms
	long forceToPosSwitchPosOffset = -10;         // 力控切换运控时，规划位置相对于编码器位置的偏移量,单位: 脉冲

	/*-------------------------------------快进阶段、搜索阶段、脱离阶段，加速度设置----------------------------------------------*/
	double acc = 2;
	double dec = 2;

	//清除驱动器报警标志、跟随误差越限标志、限位触发标志。
	gRtn = GTN_ClrSts(gCore,gAxis);
	//CommandHandler("GTN_ClrSts",gRtn);

	//-----------------------------------------------------快进阶段，开始运动-----------------------------------------------------
	TMoveAbsolutePrm moveAbsolutePrm;
	memset(&moveAbsolutePrm,0,sizeof(moveAbsolutePrm));
	moveAbsolutePrm.pos = gPosFastMoveEnd;
	moveAbsolutePrm.vel = gVelFastMove;
	moveAbsolutePrm.acc = acc;
	moveAbsolutePrm.dec = dec;
	moveAbsolutePrm.percent = 100;
	gRtn = GTN_MoveAbsolute(gCore,gAxis,&moveAbsolutePrm);

	gStage = "快进阶段";
	long sts;

	//打印运动信息
	do 
	{
		gRtn = GTN_GetSts(gCore,gAxis,&sts);
	}while(sts & 0x400); // 等待AXIS轴规划停止
	
	//-----------------------------------------------------搜索阶段，慢速运动-----------------------------------------------------
	TSwitchLoopParameter switchPrm;
	switchPrm.enable = posToForceSwitchEnable;							 //使能或者禁用环路切换功能，0：禁用环路切换功能，1：启用环路切换功能。
	switchPrm.mode = posToForceSwitchmode;								 //切换模式,0: 由运控切换至开环力控,1: 由力控切换至运控；2:由运控切换到双闭环力控
	switchPrm.dir = posToForceSwitchDir;										 //闭环切开环时阈值的判断方向，0：小于阈值时进行切换，1：大于阈值时进行切换。
	switchPrm.threshold = posToForceSwitchThreshold;					 //环路切换的阈值,位置环跟随误差超过该阈值时切换环路，单位：脉冲。
	switchPrm.openLoopDac = posToForceSwitchOpenLoopDac;   //从闭环切换到开环之后的dac输出值，单位：dac的bit位。
	switchPrm.overTime = posToForceSwitch0verTime;					//环路切换跟随误差超时时间,当跟随误差超过阈值并持续该时间后,进行切换，单位: ms
	switchPrm.posOffset = posToForceSwitchPosOffset;				//开环切换闭环时，规划位置相对于编码器位置的偏移量，单位：脉冲。只有当mode设为1时，此参数才生效。
	switchPrm.reserve[0] = 0;
	switchPrm.reserve[1] = 0;
	//设置环路模式从双闭环模式切换到力闭环模式的条件
	gRtn = GTN_SetSwitchLoopParameter(gCore,gAxis,&switchPrm);

	gStage = "搜索阶段";

	moveAbsolutePrm.pos = gPosSearchEnd;
	moveAbsolutePrm.vel = gVelSearchMove;
	moveAbsolutePrm.acc = acc;
	moveAbsolutePrm.dec = dec;
	moveAbsolutePrm.percent = 100;
	gRtn = GTN_MoveAbsolute(gCore,gAxis,&moveAbsolutePrm);

	TSwitchLoopParameter getSwitchPrm;
	do 
	{
		gRtn = GTN_GetSts(gCore,gAxis,&sts);
		//获取环路切换参数，当环路切换成功时，getSwitchPrm.enable会从1变成0
		gRtn = GTN_GetSwitchLoopParameter(gCore,gAxis,&getSwitchPrm);
		if(0 == getSwitchPrm.enable)
		{
			printf("\n");
			GTN_Stop(gCore,gAxis,1);
			gStage = "力控阶段";
			//GetDlgItem(IDC_STATIC_command)->SetWindowText(gStage);
			gRtn = GTN_GetSts(gCore,gAxis,&sts);
			break;
		}
	}while(sts & 0x400); // 等待运控切换为力控
	printf("\n");

	//-----------------------------------------------------力控阶段----------------------------------------------------------
	if ("力控阶段" ==  gStage)
	{
		Sleep(gTimeForceSustain);
		GTN_SetForceValue(gCore,gAxis,1,gForceTarget2);
		Sleep(gTimeForceSustain);
		GTN_SetForceValue(gCore,gAxis,1,gForceTarget3);
		Sleep(gTimeForceSustain);

		switchPrm.enable = forceToPosSwitchEnable;								//使能或者禁用环路切换功能，0：禁用环路切换功能，1：启用环路切换功能。
		switchPrm.mode = forceToPosSwitchmode;									//切换模式,0: 由运控切换至开环力控,1: 由力控切换至运控；2:由运控切换到双闭环力控
		switchPrm.dir = forceToPosSwitchDir;											//闭环切开环时阈值的判断方向，0：小于阈值时进行切换，1：大于阈值时进行切换。
		switchPrm.threshold = forceToPosSwitchThreshold;						//环路切换的阈值,位置环跟随误差超过该阈值时切换环路，单位：脉冲。
		switchPrm.openLoopDac = forceToPosSwitchOpenLoopDac;		//从闭环切换到开环之后的dac输出值，单位：dac的bit位。
		switchPrm.overTime = forceToPosSwitch0verTime;						//当跟随误差满足dir条件时间大于overtime时 进行切换
		switchPrm.posOffset = forceToPosSwitchPosOffset;					//开环切换闭环时，规划位置相对于编码器位置的偏移量，单位：脉冲。只有当mode设为1时，此参数才生效。
		gRtn = GTN_SetSwitchLoopParameter(gCore,gAxis,&switchPrm);	

		do 
		{
			gRtn = GTN_GetSts(gCore,gAxis,&sts);
			//获取环路切换参数，当环路切换成功时，getSwitchPrm.enable会从1变成0
			gRtn = GTN_GetSwitchLoopParameter(gCore,gAxis,&getSwitchPrm);
		}while(1 == getSwitchPrm.enable); // 等待力控切换为运控
	}
	else if("搜索阶段" ==  gStage)
	{
		//GetDlgItem(IDC_STATIC_command)->SetWindowText("切换力控阶段失败");
		forceToPosSwitchEnable = 0;
		switchPrm.enable = 0;																	 //使能或者禁用环路切换功能，0：禁用环路切换功能，1：启用环路切换功能。
		switchPrm.mode = forceToPosSwitchmode;									//切换模式,0: 由运控切换至开环力控,1: 由力控切换至运控；2:由运控切换到双闭环力控
		switchPrm.dir = forceToPosSwitchDir;											//闭环切开环时阈值的判断方向，0：小于阈值时进行切换，1：大于阈值时进行切换。
		switchPrm.threshold = forceToPosSwitchThreshold;						//环路切换的阈值,位置环跟随误差超过该阈值时切换环路，单位：脉冲。
		switchPrm.openLoopDac = forceToPosSwitchOpenLoopDac;       //从闭环切换到开环之后的dac输出值，单位：dac的bit位。
		switchPrm.overTime = forceToPosSwitch0verTime;						//当跟随误差满足dir条件时间大于overtime时 进行切换
		switchPrm.posOffset = forceToPosSwitchPosOffset;					//开环切换闭环时，规划位置相对于编码器位置的偏移量，单位：脉冲。只有当mode设为1时，此参数才生效。
		gRtn = GTN_SetSwitchLoopParameter(gCore,gAxis,&switchPrm);	
	}   

	//-----------------------------------------------------脱离阶段,回退运动----------------------------------------------------------
	//脱离阶段,抬起运动到回退位置
	gStage = "脱离阶段";
	memset(&moveAbsolutePrm,0,sizeof(moveAbsolutePrm));
	moveAbsolutePrm.pos = gPosBack;
	moveAbsolutePrm.vel = gVelBack;
	moveAbsolutePrm.acc = acc;
	moveAbsolutePrm.dec = acc;
	moveAbsolutePrm.percent = 100;
	gRtn = GTN_MoveAbsolute(gCore,gAxis,&moveAbsolutePrm);
	//CommandHandler("GTN_MoveAbsolute",gRtn);

	//打印运动信息
	do 
	{
		gRtn = GTN_GetSts(gCore,gAxis,&sts);
	}while(sts & 0x400); // 等待AXIS轴规划停止

	//GetDlgItem(IDC_STATIC_command)->SetWindowText("力控流程结束");
    return 0;
}

void CFcorceControl_TestDlg::OnOK()
{
	//MFC的Enter键默认调用OnOK 函数，
	//用户在运行的时候，如果不注意按下了ENTER或者 ESC键，程序就会立刻退出，

	//为了使Enter 键无效，最简单的办法就是将的OnOK函数写成空函数
	//此时，系统自带的 确认/取消 按钮仍然可用
}


void CFcorceControl_TestDlg::OnBnClickedButtonCalculateforceavg()
{
	CString strTemp;
	double forceValue[20]={0};
	double forceSum= 0;
	double forceAvg = 0;
	double adcValue;
	double actForce;
	int count = 20;

	gRtn = GTN_GetAdc(gCore,gAdcIndex,&adcValue);
	CommandHandler("GTN_GetForceValue",gRtn);

	actForce = adcValue*gFeedbackForceK+ gFeedbackForceB;

	for(int i = 0; i<count; i++)
	{
		forceValue[i] = actForce;
		forceSum = actForce+forceSum;
	}

	forceAvg = forceSum/count;
	strTemp.Format("%.3lf",forceAvg);
	GetDlgItem(IDC_ForceAvg)->SetWindowText(strTemp);
}
