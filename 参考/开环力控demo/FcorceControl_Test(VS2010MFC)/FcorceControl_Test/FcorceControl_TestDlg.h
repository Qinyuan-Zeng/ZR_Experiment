
// FcorceControl_TestDlg.h : 头文件
//

#pragma once


// CFcorceControl_TestDlg 对话框
class CFcorceControl_TestDlg : public CDialogEx
{
// 构造
public:
	CFcorceControl_TestDlg(CWnd* pParent = NULL);	// 标准构造函数

// 对话框数据
	enum { IDD = IDD_FCORCECONTROL_TEST_DIALOG };

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);	// DDX/DDV 支持


// 实现
protected:
	HICON m_hIcon;

	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	DECLARE_MESSAGE_MAP()
public:
	void OnOK();
	void CommandHandler(char* command, short error);
	afx_msg void OnTimer(UINT_PTR nIDEvent);
	afx_msg void OnBnClickedButtonMovefrocecontrol();
	afx_msg void OnBnClickedButtonOpencard();
	short axis;
	double posFastMoveEnd;
	double posSearchEnd;
	double forceTarget1;
	double timeForceSustain;
	double feedbackForceK;
	double feedbackForceB;
	short adcIndex;
	double velFastMove;
	double velSearchMove;
	double controlforceK;
	double controlforceB;
	short forceProtect;
	double forceLimit;
	double forceKp;
	double forceKi;
	double forceKd;
	double forceTarget2;
	double forceTarget3;
	afx_msg void OnBnClickedButtonCalculateforceavg();
	double posBack;
	double velBack;
	long posToForceSwitchDir;
	long posToForceSwitchThreshold;
	double posToForceSwitch0verTime;
};

// 线程函数声明
UINT WorkerThread(LPVOID pParam);





