
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
double gFeedbackForceK;
double gFeedbackForceB;
double gControlforceK;
double gControlforceB;
short gForceProtect;
double gForceLimit;
double gForceKp;
double gForceKi;
double gForceKd;
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
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
}

void CFcorceControl_TestDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Text(pDX, IDC_EDIT1, axis);	
	gAxis = axis;
	DDX_Text(pDX, IDC_EDIT6, adcIndex);
	gAdcIndex = adcIndex;
	DDX_Text(pDX, IDC_EDIT9, velFastMove);
	gVelFastMove = velFastMove;
	DDX_Text(pDX, IDC_EDIT10, velSearchMove);
	gVelSearchMove = velSearchMove;
	DDX_Text(pDX, IDC_EDIT2, posFastMoveEnd);
	gPosFastMoveEnd = posFastMoveEnd;
	DDX_Text(pDX, IDC_EDIT3, posSearchEnd);
	gPosSearchEnd = posSearchEnd;
	DDX_Text(pDX, IDC_EDIT13, forceProtect);
	gForceProtect = forceProtect;
	DDX_Text(pDX, IDC_EDIT14, forceLimit);
	gForceLimit = forceLimit;
	DDX_Text(pDX, IDC_EDIT7, feedbackForceK);
	gFeedbackForceK = feedbackForceK;
	DDX_Text(pDX, IDC_EDIT8, feedbackForceB);
	gFeedbackForceB = feedbackForceB;
	DDX_Text(pDX, IDC_EDIT11, controlforceK);
	gControlforceK = controlforceK;
	DDX_Text(pDX, IDC_EDIT12, controlforceB);
	gControlforceB = controlforceB;
	DDX_Text(pDX, IDC_EDIT15, forceKp);
	gForceKp = forceKp;
	DDX_Text(pDX, IDC_EDIT16, forceKi);
	gForceKi = forceKi;
	DDX_Text(pDX, IDC_EDIT17, forceKd);
	gForceKd = forceKd;
	DDX_Text(pDX, IDC_EDIT4, forceTarget1);
	gForceTarget1 = forceTarget1;
	DDX_Text(pDX, IDC_EDIT18, forceTarget2);
	gForceTarget2 = forceTarget2;
	DDX_Text(pDX, IDC_EDIT19, forceTarget3);
	gForceTarget3 = forceTarget3;
	DDX_Text(pDX, IDC_EDIT5, timeForceSustain);
	gTimeForceSustain = timeForceSustain;
}

BEGIN_MESSAGE_MAP(CFcorceControl_TestDlg, CDialogEx)
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_WM_TIMER()
	ON_BN_CLICKED(IDC_BUTTON_MoveFroceControl, &CFcorceControl_TestDlg::OnBnClickedButtonMovefrocecontrol)
	ON_BN_CLICKED(IDC_BUTTON_OpenCard, &CFcorceControl_TestDlg::OnBnClickedButtonOpencard)
    ON_MESSAGE(WM_UPDATE_CONTROL, &CFcorceControl_TestDlg::OnUpdateControl)
	ON_BN_CLICKED(IDC_BUTTON_CalculateForceAvg, &CFcorceControl_TestDlg::OnBnClickedButtonCalculateforceavg)
	ON_BN_CLICKED(IDC_BUTTON_SetForceCotrolPara, &CFcorceControl_TestDlg::OnBnClickedButtonSetforcecotrolpara)
END_MESSAGE_MAP()


// CFcorceControl_TestDlg 消息处理程序

BOOL CFcorceControl_TestDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	// 设置此对话框的图标。当应用程序主窗口不是对话框时，框架将自动
	//  执行此操作
	SetIcon(m_hIcon, TRUE);			// 设置大图标
	SetIcon(m_hIcon, FALSE);		// 设置小图标

	// TODO: 在此添加额外的初始化代码

	return TRUE;  // 除非将焦点设置到控件，否则返回 TRUE
}

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
	GetDlgItem(IDC_EDIT1)->SetWindowText("1");
	GetDlgItem(IDC_EDIT6)->SetWindowText("1");
	GetDlgItem(IDC_EDIT9)->SetWindowText("10");
	GetDlgItem(IDC_EDIT10)->SetWindowText("2");
	GetDlgItem(IDC_EDIT2)->SetWindowText("-18000.00");
	GetDlgItem(IDC_EDIT3)->SetWindowText("-20000.00");
	GetDlgItem(IDC_EDIT13)->SetWindowText("10000");
	GetDlgItem(IDC_EDIT14)->SetWindowText("5000");
	GetDlgItem(IDC_EDIT7)->SetWindowText("0.5");
	GetDlgItem(IDC_EDIT8)->SetWindowText("0");
	GetDlgItem(IDC_EDIT11)->SetWindowText("0.01");
	GetDlgItem(IDC_EDIT12)->SetWindowText("0");
	GetDlgItem(IDC_EDIT15)->SetWindowText("0.12");
	GetDlgItem(IDC_EDIT16)->SetWindowText("0");
	GetDlgItem(IDC_EDIT17)->SetWindowText("0.01");
	GetDlgItem(IDC_EDIT4)->SetWindowText("1");
	GetDlgItem(IDC_EDIT18)->SetWindowText("0.8");
	GetDlgItem(IDC_EDIT19)->SetWindowText("0.6");
	GetDlgItem(IDC_EDIT5)->SetWindowText("2000");
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

	gRtn = GTN_GetForceFeedbackValue(gCore,gAxis,&actForce);
	CommandHandler("GTN_GetForceValue",gRtn);

	//actForce = adcValue*feedbackForceK+ feedbackForceB;
	strTemp.Format("%.3lf",actForce);
	GetDlgItem(IDC_ActForce)->SetWindowText(strTemp);
	
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
		GetDlgItem(IDC_AXISMODE)->SetWindowText("开环");
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

// 消息处理函数（运行在主线程，可安全操作UI）
LRESULT CFcorceControl_TestDlg::OnUpdateControl(WPARAM wParam, LPARAM lParam) 
{
    // 1. 解析传递的数据结构
    ControlUpdateData* pData = (ControlUpdateData*)lParam;
    if (pData == nullptr) return 0;

    // 2. 获取目标控件
    CWnd* pCtrl = GetDlgItem(pData->ctrlId);
    if (pCtrl == nullptr) {
        // 释放内存并返回（避免泄漏）
        if (pData->dataType == TYPE_STRING) {
            delete pData->data.strVal;
        }
        delete pData;
        return 0;
    }

    // 3. 根据数据类型转换为字符串（UI控件通常显示字符串）
    CString strValue;
    switch (pData->dataType) {
        case TYPE_INT:
            strValue.Format(_T("%d"), pData->data.intVal);
            break;
        case TYPE_DOUBLE:
            strValue.Format(_T("%.4f"), pData->data.doubleVal); // 保留4位小数
            break;
        case TYPE_FLOAT:
            strValue.Format(_T("%.2f"), pData->data.floatVal);  // 保留2位小数
            break;
        case TYPE_STRING:
            strValue = *(pData->data.strVal);
            delete pData->data.strVal; // 释放动态分配的字符串
            break;
        default:
            strValue = _T("未知数据");
            break;
    }

    // 4. 更新控件内容
    pCtrl->SetWindowText(strValue);

    // 5. 释放数据结构内存
    delete pData;

    return 0;
}
void CFcorceControl_TestDlg::OnOK()
{
	//MFC的Enter键默认调用OnOK 函数，
	//用户在运行的时候，如果不注意按下了ENTER或者 ESC键，程序就会立刻退出，

	//为了使Enter 键无效，最简单的办法就是将的OnOK函数写成空函数
	//此时，系统自带的 确认/取消 按钮仍然可用
}


void CFcorceControl_TestDlg::OnBnClickedButtonSetforcecotrolpara()
{
	// TODO: 在此添加控件通知处理程序代码
	
	/*-----------------------------------------------------双闭环参数设置-----------------------------------------------------*/
	short index = 1;                              //闭环参数索引号
	/*-----------------------------------------------------力闭环参数设置-----------------------------------------------------*/
	double forcePid_kp = gForceKp;						 //力闭环比例增益
	double forcePid_ki = gForceKi;                       //力闭环积分增益
	double forcePid_kd = gForceKd;                       //力闭环微分增益
	short forceOutputLimit = gForceProtect;              //力闭环输出保护上限，超过保护上限轴会下使能

	short controlType = 2;                               // 控制类型,0:开环力控,1:闭环力控，2:双闭环力控
	short feedbackType = 0;                              // 反馈信号类型,0:模拟量输入
	short feedbackSrcIndex = gAdcIndex;                  // 反馈信号索引,从1开始
	double feedbackForceK = gFeedbackForceK;             // 力反馈信号的比例系数,单位: unit/V,y = k*(x + b),x是模拟量输入电压值,y是反馈力大小
	double feedbackForceB = gFeedbackForceB;             // 力反馈信号的常量系数,单位: V
	double controlforceK = gControlforceK;               // 力控输出端的比例系数,单位: bit/unit
	double controlforceB = gControlforceB;               // 力控输出端的常量系数,单位: bit
	double outputLimit = gForceLimit;                    // 输出限制,单位: unit

	long forceValue = gForceTarget1;                        //运控切力控所需输出的力，需要确认

	/*--------------------------------------------------接触前环路切换参数-----------------------------------------------------*/
	short posToForceSwitchEnable = 1;                      // 使能或者禁用环路切换功能,0: 禁用环路切换功能,1: 启用环路切换功能
	short posToForceSwitchmode = 2;                        // 切换模式,0: 由运控切换至力控,1: 由力控切换至运控,2:由运控切换到双闭环力控
	long posToForceSwitchDir = 1;                          // 运控切力控时阈值的判断方向,0: 小于阈值时进行切换,1: 大于阈值时进行切换
	long posToForceSwitchThreshold = 1;                    // 环路切换的阈值,位置环跟随误差超过该阈值时切换环路,单位: 脉冲
	long posToForceSwitchOpenLoopDac = forceValue;         // 从运控切换到力控之后的输出值,单位: 标定后的实际力单位
	double posToForceSwitch0verTime = 1;                   // 环路切换跟随误差超时时间,当跟随误差超过阈值并持续该时间后,进行切换
	long posToForceSwitchPosOffset = 0;                    // 力控切换运控时，规划位置相对于编码器位置的偏移量,单位: 脉冲。只有当mode设为1时，此参数才生效。

	//清除驱动器报警标志、跟随误差越限标志、限位触发标志。
	gRtn = GTN_ClrSts(gCore, gAxis);
	CommandHandler("GTN_ClrSts",gRtn);

	//设置力闭环环路参数
	TPid forcePid;
	forcePid.kp = forcePid_kp;                            //力闭环比例增益
	forcePid.ki = forcePid_ki;                            //力闭环积分增益
	forcePid.kd = forcePid_kd;                            //力闭环微分增益
	forcePid.kaff = 0.0;
	forcePid.kvff = 0.0;
	forcePid.integralLimit = 32767;
	forcePid.derivativeLimit = 32767;
	forcePid.limit = forceOutputLimit;                    //力闭环输出保护上限，超过保护上限轴会下使能
	gRtn = GTN_SetForceLoopPid(gCore, gAxis, index, &forcePid);
	CommandHandler("GTN_SetForceLoopPid",gRtn);

	//力闭环补偿表函数
	short count = 5;
	TForceFeedbackCompensatePara pPara[5];
	pPara[0].pVoltageRefValue = -10.00;
	pPara[1].pVoltageRefValue = -1.00;
	pPara[2].pVoltageRefValue = 0;
	pPara[3].pVoltageRefValue = 1;
	pPara[4].pVoltageRefValue = 10.00;
	pPara[0].pForceCompValue = -32767;
	pPara[1].pForceCompValue = -3276.7;
	pPara[2].pForceCompValue = 0;
	pPara[3].pForceCompValue = 3276.7;
	pPara[4].pForceCompValue = 32767;
	gRtn = GTN_SetForceFeedbackComp(gCore, gAxis, pPara,count);
	CommandHandler("GTN_SetForceFeedbackComp",gRtn);

	//获取力闭环补偿表
	TForceFeedbackCompensatePara pParaGet[5];
	gRtn = GTN_GetForceFeedbackComp(gCore, gAxis, pParaGet,&count);
	CommandHandler("GTN_GetForceFeedbackComp",gRtn);

	//设置力闭环反馈信号索引，标定参数等
	TForceControlPrm forceControlPrm;
	forceControlPrm.controlType = controlType;				// 控制类型,0:开环力控,1:闭环力控
	forceControlPrm.feedbackType = MC_ADC;					// 反馈信号类型,0:模拟量输入
	forceControlPrm.feedbackSrcIndex = feedbackSrcIndex;    // 反馈信号索引,从1开始
	forceControlPrm.feedbackForceK = feedbackForceK;        // 力反馈信号的比例系数,单位: unit/V
	forceControlPrm.feedbackForceB = feedbackForceB;        // 力反馈信号的常量系数,单位: V
	forceControlPrm.controlforceK = controlforceK;          // 力控输出端的比例系数,单位: bit/unit
	forceControlPrm.controlforceB = controlforceB;          // 力控输出端的常量系数,单位: bit
	forceControlPrm.outputLimit = outputLimit;              // 输出限制,单位: unit
	forceControlPrm.outputReverse = -1;					    // 力控输出是否取反, 1:正常;-1:取反
	forceControlPrm.calibrationMode = 2;				    // 标定模式，1：使用y=feedbackforceK*x +feedbackforceB标定 ；2：使用GTN_SetForceFeedbackComp补偿表标定
	gRtn = GTN_SetForceControlParameter(gCore, gAxis, &forceControlPrm);
	CommandHandler("GTN_SetForceControlParameter",gRtn);

	//获取闭环力控参数
	gRtn = GTN_GetForceControlParameter(gCore, gAxis, &forceControlPrm);
	CommandHandler("GTN_GetForceControlParameter",gRtn);

	TSwitchLoopParameter switchPrm;
	switchPrm.enable = posToForceSwitchEnable;            //使能或者禁用环路切换功能，0：禁用环路切换功能，1：启用环路切换功能。
	switchPrm.mode = posToForceSwitchmode;			   	  //切换模式，0: 由运控切换至力控,1: 由力控切换至运控,2：由运控到双闭环力控
	switchPrm.dir = posToForceSwitchDir;				  //闭环切开环时阈值的判断方向，0：小于阈值时进行切换，1：大于阈值时进行切换。
	switchPrm.threshold = posToForceSwitchThreshold;      //环路切换的阈值,位置环跟随误差超过该阈值时切换环路，单位：脉冲。
	switchPrm.openLoopDac = posToForceSwitchOpenLoopDac;  //从闭环切换到开环之后的dac输出值，单位：dac的bit位。
	switchPrm.overTime = posToForceSwitch0verTime;	      //当跟随误差满足dir条件时间大于overtime时 进行切换
	switchPrm.posOffset = posToForceSwitchPosOffset;      //开环切换闭环时，规划位置相对于编码器位置的偏移量，单位：脉冲。只有当mode设为1时，此参数才生效。
	switchPrm.reserve[0] = 0;
	switchPrm.reserve[1] = 0;
	//设置环路模式从双闭环模式切换到力闭环模式的条件
	gRtn = GTN_SetSwitchLoopParameter(gCore, gAxis, &switchPrm);
	CommandHandler("GTN_SetSwitchLoopParameter",gRtn);
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
	/*--------------------------------------------------脱离工件前环路切换参数-----------------------------------------------------*/
	short forceToPosSwitchEnable = 1;                      // 使能或者禁用环路切换功能,0: 禁用环路切换功能,1: 启用环路切换功能
	short forceToPosSwitchmode = 1;                        //切换模式，0: 由运控切换至力控,1: 由力控切换至运控,2：由运控到双闭环力控
	long forceToPosSwitchDir = 1;                          // 运控切力控时阈值的判断方向,0: 小于阈值时进行切换,1: 大于阈值时进行切换
	long forceToPosSwitchThreshold = 15;                   // 环路切换的阈值,位置环跟随误差超过该阈值时切换环路,单位: 脉冲
	long forceToPosSwitchOpenLoopDac =10;                  // 从运控切换到力控之后的dac输出值,单位: dac的bit位
	double forceToPosSwitch0verTime = 1;                   // 环路切换跟随误差超时时间,当跟随误差超过阈值并持续该时间后,进行切换
	long forceToPosSwitchPosOffset = -10;                  // 力控切换运控时，规划位置相对于编码器位置的偏移量,单位: 脉冲

	/*-------------------------------------快进阶段与搜索阶段，目标位置与速度设置----------------------------------------------*/
	double acc = 2;
	double dec = 2;

	/*------------------------------------------脱离阶段，目标位置与速度设置---------------------------------------------------*/
	double velBack = 10;
	long posBack = 0 ;



	TSwitchLoopParameter switchPrm;
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
	//GetDlgItem(IDC_STATIC_command)->SetWindowText(gStage);
	long sts;

	//打印运动信息
	do 
	{
		gRtn = GTN_GetSts(gCore,gAxis,&sts);
	}while(sts & 0x400); // 等待AXIS轴规划停止
		
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

		switchPrm.enable = forceToPosSwitchEnable;               //使能或者禁用环路切换功能，0：禁用环路切换功能，1：启用环路切换功能。
		switchPrm.mode = forceToPosSwitchmode;				     //切换模式，0：由闭环切换至开环，1：由开环切换至闭环。
		switchPrm.dir = forceToPosSwitchDir;				     //闭环切开环时阈值的判断方向，0：小于阈值时进行切换，1：大于阈值时进行切换。
		switchPrm.threshold = forceToPosSwitchThreshold;         //环路切换的阈值,位置环跟随误差超过该阈值时切换环路，单位：脉冲。
		switchPrm.openLoopDac = forceToPosSwitchOpenLoopDac;     //从闭环切换到开环之后的dac输出值，单位：dac的bit位。
		switchPrm.overTime = forceToPosSwitch0verTime;		     //当跟随误差满足dir条件时间大于overtime时 进行切换
		switchPrm.posOffset = forceToPosSwitchPosOffset;         //开环切换闭环时，规划位置相对于编码器位置的偏移量，单位：脉冲。只有当mode设为1时，此参数才生效。
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
		switchPrm.enable = 0;                                    //使能或者禁用环路切换功能，0：禁用环路切换功能，1：启用环路切换功能。
		switchPrm.mode = forceToPosSwitchmode;				     //切换模式，0: 由运控切换至力控,1: 由力控切换至运控,2：由运控到双闭环力控
		switchPrm.dir = forceToPosSwitchDir;				     //闭环切开环时阈值的判断方向，0：小于阈值时进行切换，1：大于阈值时进行切换。
		switchPrm.threshold = forceToPosSwitchThreshold;         //环路切换的阈值,位置环跟随误差超过该阈值时切换环路，单位：脉冲。
		switchPrm.openLoopDac = forceToPosSwitchOpenLoopDac;     //从闭环切换到开环之后的dac输出值，单位：dac的bit位。
		switchPrm.overTime = forceToPosSwitch0verTime;		     //当跟随误差满足dir条件时间大于overtime时 进行切换
		switchPrm.posOffset = forceToPosSwitchPosOffset;         //开环切换闭环时，规划位置相对于编码器位置的偏移量，单位：脉冲。只有当mode设为1时，此参数才生效。
		gRtn = GTN_SetSwitchLoopParameter(gCore,gAxis,&switchPrm);	
	}   

	//-----------------------------------------------------脱离阶段,回退运动----------------------------------------------------------
	//脱离阶段,抬起运动到回退位置
	gStage = "脱离阶段";
	memset(&moveAbsolutePrm,0,sizeof(moveAbsolutePrm));
	moveAbsolutePrm.pos = posBack;
	moveAbsolutePrm.vel = gVelSearchMove;
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

	actForce = adcValue*gFeedbackForceK+ gFeedbackForceK;

	for(int i = 0; i<count; i++)
	{
		forceValue[i] = actForce;
		forceSum = actForce+forceSum;
	}

	forceAvg = forceSum/count;
	strTemp.Format("%.3lf",forceAvg);
	GetDlgItem(IDC_ForceAvg)->SetWindowText(strTemp);
}


