#include "pch.h"
#include "Math.h"
#include "DataBase.h"
#include "Function.h"
#include "TCPSocket.h"
#include "OleDBErr.h"
#include "ConfigManage.h"
#include "GameLogManage.h"
#include "log.h"

//处理线程结构定义
struct DBThreadParam
{
	HANDLE						hEvent;									//退出事件
	HANDLE						hCompletionPort;						//完成端口
	CDataBaseManage			* pDataManage;							//数据库管理类指针
};

CDataBaseManage::CDataBaseManage()
{
	m_bInit = false;
	m_bRun = false;
	m_hThread = NULL;
	m_hCompletePort = NULL;
	m_pInitInfo = NULL;
	m_pKernelInfo = NULL;
	m_pHandleService = NULL;
	m_pMysqlHelper = NULL;

	m_sqlClass = 0;
	m_nPort = 0;
	m_bsqlInit = FALSE;

	m_host[0] = '\0';
	m_user[0] = '\0';
	m_name[0] = '\0';
	m_passwd[0] = '\0';
}

CDataBaseManage::~CDataBaseManage()
{
	m_bInit = false;
	m_pInitInfo = NULL;
	m_hThread = NULL;
	m_hCompletePort = NULL;
	m_pKernelInfo = NULL;
	m_pHandleService = NULL;
}

//开始服务
bool CDataBaseManage::Start()
{
	INFO_LOG("DataBaseManage start begin ...");

	if (m_bRun == true || m_bInit == false)
	{
		ERROR_LOG("DataBaseManage already running or not inited m_bRun=%d m_bInit=%d", m_bRun, m_bInit);
		return false;
	}

	m_bRun = true;

	//建立事件
	HANDLE StartEvent = CreateEvent(FALSE, TRUE, NULL, NULL);

	//建立完成端口
	m_hCompletePort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);
	if (m_hCompletePort == NULL)
	{
		ERROR_LOG("CreateIoCompletionPort failed err=%d", GetLastError());
		return false;
	}

	m_DataLine.SetCompletionHandle(m_hCompletePort);

	SQLConnectReset();
	//SQLConnect();

	//建立数据处理线程
	unsigned dwThreadID = 0;
	DBThreadParam ThreadStartData;

	ThreadStartData.pDataManage = this;
	ThreadStartData.hEvent = StartEvent;
	ThreadStartData.hCompletionPort = m_hCompletePort;

	int roomID = 0;
	if (m_pInitInfo)
	{
		roomID = m_pInitInfo->uRoomID;
	}

	m_hThread = (HANDLE)_beginthreadex(NULL, 0, DataServiceThread, &ThreadStartData, 0, &dwThreadID);
	if (m_hThread == NULL)
	{
		ERROR_LOG("_beginthreadex DataServiceThread failed");
		return false;
	}

	// 关联日志文件
	GameLogManage()->AddLogFile(dwThreadID, THREAD_TYPE_ASYNC, roomID);
	// 等待子线程读取线程参数
	WaitForSingleObject(StartEvent, INFINITE);

	ResetEvent(StartEvent);

	INFO_LOG("DataBaseManage start end.");

	return true;
}

bool CDataBaseManage::Stop()
{
	INFO_LOG("DataBaseManage stop begin...");

	if (!m_bRun)
	{
		ERROR_LOG("DataBaseManage is not running...");
		return false;
	}

	m_bRun = false;


	// 先关闭完成端口
	if (m_hCompletePort)
	{
		PostQueuedCompletionStatus(m_hCompletePort, 0, NULL, NULL);
		CloseHandle(m_hCompletePort);
		m_hCompletePort = NULL;
	}

	// 清理dataline
	m_DataLine.SetCompletionHandle(NULL);
	m_DataLine.CleanLineData();

	// 关闭数据库处理线程句柄
	if (m_hThread)
	{
		WaitForSingleObject(m_hThread, INFINITE);
		CloseHandle(m_hThread);
		m_hThread = NULL;
	}

	//关闭数据库连接
	if (m_pMysqlHelper)
	{
		m_pMysqlHelper->disconnect();
		delete m_pMysqlHelper;
		m_pMysqlHelper = NULL;
	}

	INFO_LOG("DataBaseManage stop end.");

	return true;
}

//取消初始化
bool CDataBaseManage::UnInit()
{
	return true;
}

//加入处理队列
bool CDataBaseManage::PushLine(DataBaseLineHead * pData, UINT uSize, UINT uHandleKind, UINT uIndex, DWORD dwHandleID)
{
	//处理数据
	pData->dwHandleID = dwHandleID;
	pData->uIndex = uIndex;
	pData->uHandleKind = uHandleKind;
	return m_DataLine.AddData(&pData->DataLineHead, uSize, 0) != 0;
}

//数据库处理线程
unsigned __stdcall CDataBaseManage::DataServiceThread(LPVOID pThreadData)
{
	Sleep(1);

	INFO_LOG("DataServiceThread starting...");

	//数据定义
	DBThreadParam		* pData = (DBThreadParam *)pThreadData;		//线程启动数据指针
	CDataBaseManage		* pDataManage = pData->pDataManage;				//数据库管理指针
	CDataLine				* pDataLine = &pDataManage->m_DataLine;			//数据队列指针
	IDataBaseHandleService	* pHandleService = pDataManage->m_pHandleService;	//数据处理接口
	HANDLE					hCompletionPort = pData->hCompletionPort;			//完成端口

	//线程数据读取完成
	::SetEvent(pData->hEvent);

	//重叠数据
	void					* pIOData = NULL;									//数据
	DWORD					dwThancferred = 0;								//接收数量
	ULONG_PTR				dwCompleteKey = 0L;								//重叠 IO 临时数据
	LPOVERLAPPED			OverData;										//重叠 IO 临时数据

	//数据缓存
	BOOL					bSuccess = FALSE;
	BYTE					szBuffer[LD_MAX_PART];

	while (pDataManage->m_bRun == true)
	{
		//等待完成端口
		bSuccess = GetQueuedCompletionStatus(hCompletionPort, &dwThancferred, &dwCompleteKey, (LPOVERLAPPED *)&OverData, INFINITE);
		if (bSuccess == FALSE || dwThancferred == 0)
		{
			//_endthreadex(0);
			continue;
		}

		while (pDataLine->GetDataCount())
		{
			try
			{
				//处理完成端口数据
				if (pDataLine->GetData((DataLineHead *)szBuffer, sizeof(szBuffer)) < sizeof(DataBaseLineHead))
				{
					continue;
				}

				pHandleService->HandleDataBase((DataBaseLineHead *)szBuffer);
			}
			catch (...)
			{
				ERROR_LOG("CATCH:%s with %s\n", __FILE__, __FUNCTION__);
				continue;
			}
		}
	}

	INFO_LOG("DataServiceThread exit.");

	return 0;
}

//重联数据库
bool CDataBaseManage::SQLConnectReset()
{
	if (m_bsqlInit == FALSE)
	{
		const DBConfig& dbConfig = ConfigManage()->GetDBConfig();

		m_nPort = dbConfig.port;

		strcpy(m_host, dbConfig.ip);
		strcpy(m_user, dbConfig.user);
		strcpy(m_name, dbConfig.dbName);
		strcpy(m_passwd, dbConfig.passwd);

		//m_szDetectTable = TEXT("");
		m_bsqlInit = TRUE;
	}

	if (m_pMysqlHelper)
	{
		delete m_pMysqlHelper;
	}

	m_pMysqlHelper = new CMysqlHelper;

	//初始化mysql对象并建立连接
	m_pMysqlHelper->init(m_host, m_user, m_passwd, m_name, "", m_nPort);
	try
	{
		m_pMysqlHelper->connect();
	}
	catch (MysqlHelper_Exception& excep)
	{
		ERROR_LOG("连接数据库失败:%s", excep.errorInfo.c_str());
		return false;
	}

	return true;
}

bool CDataBaseManage::CheckSQLConnect()
{
	if (!m_pMysqlHelper)
	{
		return false;
	}

	char buf[128] = "";
	sprintf(buf, "select * from %s LIMIT 1", TBL_BASE_GAME);
	bool bConect = false;

	try
	{
		m_pMysqlHelper->sqlExec(buf);
	}
	catch (...)
	{
		bConect = true;
	}

	if (bConect)
	{
		try
		{
			m_pMysqlHelper->connect();
		}
		catch (MysqlHelper_Exception& excep)
		{
			ERROR_LOG("连接数据库失败:%s", excep.errorInfo.c_str());
			return false;
		}
	}

	return true;
}

//***********************************************************************************************//
CDataBaseHandle::CDataBaseHandle()
{
	m_pInitInfo = NULL;
	m_pKernelInfo = NULL;
	m_pRusultService = NULL;
	m_pDataBaseManage = NULL;
}

CDataBaseHandle::~CDataBaseHandle()
{
}

bool CDataBaseHandle::SetParameter(IAsynThreadResultService * pRusultService, CDataBaseManage * pDataBaseManage, ManageInfoStruct * pInitData, KernelInfoStruct * pKernelData)
{
	m_pInitInfo = pInitData;
	m_pKernelInfo = pKernelData;
	m_pRusultService = pRusultService;
	m_pDataBaseManage = pDataBaseManage;

	return true;
}

//初始化函数
bool CDataBaseManage::Init(ManageInfoStruct* pInitInfo, KernelInfoStruct * pKernelInfo, IDataBaseHandleService * pHandleService, IAsynThreadResultService * pResultService)
{
	if (!pInitInfo || !pKernelInfo || !pHandleService || !pResultService)
	{
		throw new CException(TEXT("CDataBaseManage::Init 参数错误!"), 0x407);
	}

	//效验参数
	if (m_bInit == true || m_bRun == true)
	{
		throw new CException(TEXT("CDataBaseManage::Init 状态效验失败"), 0x408);
	}

	//设置数据
	m_pInitInfo = pInitInfo;
	m_pKernelInfo = pKernelInfo;
	m_pHandleService = pHandleService;
	m_DataLine.CleanLineData();

	//设置数据
	m_bInit = true;

	return true;
}
