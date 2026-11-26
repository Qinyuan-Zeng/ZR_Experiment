
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

	// 生成的消息映射函数
	virtual BOOL OnInitDialog();
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
    afx_msg LRESULT OnUpdateControl(WPARAM wParam, LPARAM lParam);
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
	afx_msg void OnBnClickedButtonSetforcecotrolpara();
};

// 线程函数声明
UINT WorkerThread(LPVOID pParam);
// 数据类型枚举（标识传递的数据类型）
enum DataType {
    TYPE_INT,       // 整数
    TYPE_DOUBLE,    // 双精度浮点数
    TYPE_FLOAT,     // 单精度浮点数
    TYPE_STRING     // 字符串
    // 可根据需要扩展其他类型
};

// 用于传递数据的结构体
struct ControlUpdateData {
    UINT ctrlId;         // 目标控件ID（如IDC_EDIT1、IDC_STATIC2等）
    DataType dataType;   // 数据类型
    union {              // 存储不同类型的数据（联合体）
        int intVal;
        double doubleVal;
        float floatVal;
        CString* strVal; // 字符串需用指针（联合体中不能直接放CString）
    } data;
};

// 自定义消息（用于通知更新控件）
#define WM_UPDATE_CONTROL (WM_USER + 200)

