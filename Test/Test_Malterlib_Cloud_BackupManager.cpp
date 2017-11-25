#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Concurrency/DistributedAppInterfaceLaunch>
#include <Mib/Concurrency/DistributedTrustTestHelpers>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Concurrency/DistributedActorTrustManagerProxy>
#include <Mib/Concurrency/DistributedAppTestHelpers>
#include <Mib/Concurrency/DistributedActorTestHelpers>
#include <Mib/Cloud/BackupManager>
#include <Mib/Cloud/BackupManagerClient>
#include <Mib/Cloud/BackupManagerDownload>
#include <Mib/Cloud/App/BackupManager>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Test/Exception>

using namespace NMib;
using namespace NMib::NConcurrency;
using namespace NMib::NFile;
using namespace NMib::NStr;
using namespace NMib::NProcess;
using namespace NMib::NContainer;
using namespace NMib::NCryptography;
using namespace NMib::NCloud;
using namespace NMib::NPtr;
using namespace NMib::NAtomic;
using namespace NMib::NEncoding;
using namespace NMib::NStorage;
using namespace NMib::NTime;
using namespace NMib::NNet;

#define DTestBackupManagerEnableLogging 0

static fp64 g_Timeout = NSys::fg_System_BeingDebugged() ? 600.0 : 60.0;

class CBackupManager_Tests : public NMib::NTest::CTest
{
public:
	void f_DoTests()
	{
		DMibTestSuite("General")
		{
			fp_DoGeneralTests();
		};
	}

	struct COtherNotifications
	{
		void f_Clear()
		{
			m_nNotifications.f_Clear();
			m_Notifications.f_Clear();
			m_LastNotifications.f_Clear();
		}

		mint f_ClaimNotifications(CBackupManagerClient::ENotification _Type)
		{
			mint nNotifications = m_nNotifications[_Type];
			m_nNotifications.f_Remove(_Type);
			return nNotifications;
		}

		TCSet<CStr> f_UnclaimedNotificaions()
		{
			TCSet<CStr> Unclaimed;
			for (auto &nNotifications : m_nNotifications)
			{
				auto NotificationsType = m_nNotifications.fs_GetKey(nNotifications);
				CStr Desc;
				switch (NotificationsType)
				{
				case CBackupManagerClient::ENotification_BackupAborted: Desc = "BackupAborted {}"_f << nNotifications; break;
				case CBackupManagerClient::ENotification_BackupError: Desc = "BackupError {}"_f << nNotifications; break;
				case CBackupManagerClient::ENotification_FileFinished: Desc = "FileFinished {}"_f << nNotifications; break;
				case CBackupManagerClient::ENotification_Quiescent: Desc = "Quiescent {}"_f << nNotifications; break;
				case CBackupManagerClient::ENotification_Unquiescent: Desc = "Unquiescent {}"_f << nNotifications; break;
				case CBackupManagerClient::ENotification_InitialFinished: Desc = "InitialFinished {}"_f << nNotifications; break;
				}
				Unclaimed[Desc];
			}

			return Unclaimed;
		}

		template <CBackupManagerClient::ENotification tf_Notification>
		auto f_LastNotification()
		{
			if (auto *pLast = m_LastNotifications.f_FindEqual(tf_Notification))
				return pLast->f_Get<tf_Notification>();
			DMibError("No notification found");
		}

		TCSet<CStr> f_InitialAddedFiles()
		{
			TCSet<CStr> Return;
			for (auto &File : f_LastNotification<CBackupManagerClient::ENotification_InitialFinished>().m_AddedFiles)
				Return[File];
			return Return;
		}

		TCSet<CStr> f_InitialDeletedFiles()
		{
			TCSet<CStr> Return;
			for (auto &File : f_LastNotification<CBackupManagerClient::ENotification_InitialFinished>().m_RemovedFiles)
				Return[File];
			return Return;
		}

		TCSet<CStr> f_InitialUpdatedFiles()
		{
			TCSet<CStr> Return;
			for (auto &File : f_LastNotification<CBackupManagerClient::ENotification_InitialFinished>().m_UpdatedFiles)
				Return[File];
			return Return;
		}

		TCMap<CBackupManagerClient::ENotification, zmint> m_nNotifications;
		TCMap<CBackupManagerClient::ENotification, CBackupManagerClient::CNotification> m_LastNotifications;
		TCVector<CBackupManagerClient::CNotification> m_Notifications;
	};

	struct CTestState
	{
		NThread::CMutual m_Lock;
		NThread::CEventAutoReset m_Event;
		TCLinkedList<CBackupManagerClient::CNotification> m_Notifications;

		template <CBackupManagerClient::ENotification tf_Notification>
		auto f_WaitForNotification(COtherNotifications &o_OtherNotifications)
		{
			o_OtherNotifications.f_Clear();
			while (true)
			{
				{
					DMibLock(m_Lock);
					while (!m_Notifications.f_IsEmpty())
					{
						auto &Notification = m_Notifications.f_GetFirst();
						auto Cleanup = g_OnScopeExit > [&]
							{
								m_Notifications.f_Remove(Notification);
							}
						;

						if (Notification.f_GetTypeID() == tf_Notification)
							return Notification.f_Get<tf_Notification>();
						else
						{
							++o_OtherNotifications.m_nNotifications[Notification.f_GetTypeID()];
							o_OtherNotifications.m_Notifications.f_Insert(Notification);
							o_OtherNotifications.m_LastNotifications[Notification.f_GetTypeID()] = Notification;
						}
					}
				}
				if (m_Event.f_WaitTimeout(g_Timeout / 2.0))
					DMibError("Timed out waiting for backup notification");
			}
		}
	};

	struct CBackupClientHelper
	{
		CBackupClientHelper(CStr const &_TestBackupDirectory)
		{
			m_BackupConfig.m_BackupIdentifier = "Test";
			m_BackupConfig.m_ManifestConfig.m_Root = _TestBackupDirectory;
			m_BackupConfig.m_ChangeAggregationTime = 0.05;
			m_BackupConfig.m_bReportChangesInInitialFinished = true;
		}

		~CBackupClientHelper()
		{
			if (m_BackupClient)
				m_BackupClient->f_BlockDestroy();
		}

		template <CBackupManagerClient::ENotification tf_Notification>
		auto f_WaitForNotification(COtherNotifications &o_OtherNotifications)
		{
			return m_pState->f_WaitForNotification<tf_Notification>(o_OtherNotifications);
		}

		CActorSubscription f_Start(TCActor<CDistributedActorTrustManager> const &_TrustManager, CDistributedApp_LaunchHelperDependencies const &_Dependencies)
		{
			TCSharedPointer<NConcurrency::CActorSubscription> pManifestFinished = fg_Construct();
			TCSharedPointer<TCDistributedActorInterfaceWithID<CDistributedAppInterfaceBackup>> pBackupInterface = fg_Construct();
			TCContinuation<void> ReceivedManifestFinished;
			m_BackupClient = fg_Construct
				(
					m_BackupConfig
					, _TrustManager
					, g_ActorFunctor > [pManifestFinished, pBackupInterface, ReceivedManifestFinished]
					(
						TCDistributedActorInterfaceWithID<CDistributedAppInterfaceBackup> &&_BackupInterface
						, CActorSubscription &&_ManifestFinished
						, CStr const &_BackupRoot
					) -> TCContinuation<TCActorSubscriptionWithID<>>
					{
						*pManifestFinished = fg_Move(_ManifestFinished);
						*pBackupInterface = fg_Move(_BackupInterface);
						if (!ReceivedManifestFinished.f_IsSet())
							ReceivedManifestFinished.f_SetResult();
						return fg_Explicit
							(
								g_ActorSubscription > []
								{
								}
							)
						;
					}
					, _Dependencies.m_DistributionManager
				)
			;

			m_ChangeSubscription = m_BackupClient
				(
					&CBackupManagerClient::f_SubscribeNotifications
					, CBackupManagerClient::ENotification_BackupAborted
					| CBackupManagerClient::ENotification_BackupError
					| CBackupManagerClient::ENotification_FileFinished
					| CBackupManagerClient::ENotification_Quiescent
					| CBackupManagerClient::ENotification_Unquiescent
					| CBackupManagerClient::ENotification_InitialFinished
					, g_ActorFunctor > [pState = m_pState, ReceivedManifestFinished](NConcurrency::CHostInfo const &_RemoteHost, CBackupManagerClient::CNotification &&_Notification)
					-> NConcurrency::TCContinuation<void>
					{
#ifdef DMibCloudBackupManagerDebug
						switch (_Notification.f_GetTypeID())
						{
						case CBackupManagerClient::ENotification_BackupAborted:
							{
								DMibCloudBackupManagerDebugOut("+++ BackupAborted\n");
							}
							break;
						case CBackupManagerClient::ENotification_BackupError:
							{
								auto &Notfification = _Notification.f_Get<CBackupManagerClient::ENotification_BackupError>();
								DMibCloudBackupManagerDebugOut("+++ BackupError {}, {}\n", Notfification.m_ErrorMessage, Notfification.m_bFatal);
							}
							break;
						case CBackupManagerClient::ENotification_FileFinished:
							{
								auto &Notfification = _Notification.f_Get<CBackupManagerClient::ENotification_FileFinished>();
								DMibCloudBackupManagerDebugOut("+++ FileFinished {} {}\n", Notfification.m_FileName, Notfification.m_TransferStats);
							}
							break;
						case CBackupManagerClient::ENotification_Quiescent:
							{
								DMibCloudBackupManagerDebugOut("+++ Quiescent\n");
							}
							break;
						case CBackupManagerClient::ENotification_Unquiescent:
							{
								DMibCloudBackupManagerDebugOut("+++ Unquiescent\n");
							}
							break;
						case CBackupManagerClient::ENotification_InitialFinished:
							{
								auto &Notfification = _Notification.f_Get<CBackupManagerClient::ENotification_InitialFinished>();
								DMibCloudBackupManagerDebugOut("+++ InitialFinished {vs} {vs} {vs}\n", Notfification.m_AddedFiles, Notfification.m_RemovedFiles, Notfification.m_UpdatedFiles);
							}
							break;
						}
#endif
						DMibLock(pState->m_Lock);
						pState->m_Notifications.f_Insert(fg_Move(_Notification));
						pState->m_Event.f_Signal();
						if (_Notification.f_GetTypeID() == CBackupManagerClient::ENotification_BackupError)
						{
							auto &Error = _Notification.f_Get<CBackupManagerClient::ENotification_BackupError>();
							if (Error.m_bFatal && !ReceivedManifestFinished.f_IsSet())
								ReceivedManifestFinished.f_SetResult();
						}
						return fg_Explicit();
					}
				).f_CallSync(g_Timeout)
			;
			m_BackupClient(&CBackupManagerClient::f_StartBackup).f_CallSync(g_Timeout);
			ReceivedManifestFinished.f_CallSync(g_Timeout);

			m_BackupInterface = fg_Move(*pBackupInterface);

			NConcurrency::CActorSubscription ManifestFinished = fg_Move(*pManifestFinished);

			return ManifestFinished;
		}

		void f_CreateFile(CStr const &_Name, mint _Size)
		{
			DMibCloudBackupManagerDebugOut("Create file      {} {}\n", _Name, _Size);
			TCVector<uint8> Data;
			mint BufferSize = fg_AlignUp(_Size, 4);
			Data.f_SetLen(BufferSize);

			for (mint i = 0; i < BufferSize; i += 4)
				*((uint32 *)(Data.f_GetArray() + i)) = NMisc::fg_GetRandomUnsigned();

			Data.f_SetLen(_Size);

			CFile::fs_CreateDirectory(CFile::fs_GetPath(m_BackupConfig.m_ManifestConfig.m_Root / _Name));
			CFile::fs_WriteFile(Data, m_BackupConfig.m_ManifestConfig.m_Root / _Name);
		}

		void f_CreateSymlink(CStr const &_Name, CStr const &_Destination, bool _bRelative, bool _bDirectory)
		{
			DMibCloudBackupManagerDebugOut("Create symlink      {} = {}\n", _Name, _Destination);
			CFile::fs_CreateDirectory(CFile::fs_GetPath(m_BackupConfig.m_ManifestConfig.m_Root / _Name));
			CFile::fs_CreateSymbolicLink
				(
				 	_bRelative ? _Destination : m_BackupConfig.m_ManifestConfig.m_Root / _Destination
				 	, m_BackupConfig.m_ManifestConfig.m_Root / _Name
				 	, _bDirectory ? EFileAttrib_Directory : EFileAttrib_File
				 	, _bRelative ? ESymbolicLinkFlag_Relative : ESymbolicLinkFlag_None
				)
			;
		}

		bool f_FileIsSame(CStr const &_BackupDirectory, CStr const &_Name, CStr const &_SourceName = {})
		{
			CStr SourceName = _SourceName;
			if (SourceName.f_IsEmpty())
				SourceName = _Name;
			return CFile::fs_ReadFile(_BackupDirectory / _Name) == CFile::fs_ReadFile(m_BackupConfig.m_ManifestConfig.m_Root / SourceName);
		}

		void f_CreateDirectory(CStr const &_Name)
		{
			DMibCloudBackupManagerDebugOut("Create directory      {}\n", _Name);
			CFile::fs_CreateDirectory(m_BackupConfig.m_ManifestConfig.m_Root / _Name);
		}

		void f_DeleteFile(CStr const &_Name)
		{
			DMibCloudBackupManagerDebugOut("Delete file      {}\n", _Name);
			CFile::fs_DeleteFile(m_BackupConfig.m_ManifestConfig.m_Root / _Name);
		}

		void f_SwitchFile(CStr const &_File1, CStr const &_File2, CStr const &_Temp)
		{
			DMibCloudBackupManagerDebugOut("Switch file      {} <-> {}   -- ({})\n", _File1, _File2, _Temp);
			CFile::fs_RenameFile(m_BackupConfig.m_ManifestConfig.m_Root / _File1, m_BackupConfig.m_ManifestConfig.m_Root / _Temp);
			CFile::fs_RenameFile(m_BackupConfig.m_ManifestConfig.m_Root / _File2, m_BackupConfig.m_ManifestConfig.m_Root / _File1);
			CFile::fs_RenameFile(m_BackupConfig.m_ManifestConfig.m_Root / _Temp, m_BackupConfig.m_ManifestConfig.m_Root / _File2);
		}

		CStr f_GetPath(CStr const &_Path)
		{
			return m_BackupConfig.m_ManifestConfig.m_Root / _Path;
		}

		void f_RenameFile(CStr const &_FromName, CStr const &_ToName)
		{
			DMibCloudBackupManagerDebugOut("Rename file      {} -> {}\n", _FromName, _ToName);
			CFile::fs_RenameFile(m_BackupConfig.m_ManifestConfig.m_Root / _FromName, m_BackupConfig.m_ManifestConfig.m_Root / _ToName);
		}

		void f_ModifyFile(CStr const &_Name, mint _Pos)
		{
			CFile File;
			File.f_Open(m_BackupConfig.m_ManifestConfig.m_Root / _Name, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);

			uint8 Data;
			File.f_SetPosition(_Pos);
			File.f_Read(&Data, 1);

			Data = ~Data;
			File.f_SetPosition(_Pos);
			File.f_Write(&Data, 1);
		}

		void f_RandomModifyFile(CStr const &_Name, NMisc::CRandomShiftRNG &_Random)
		{
			DMibCloudBackupManagerDebugOut("RandomModifyFile {}\n", _Name);
			CFile File;
			File.f_Open(m_BackupConfig.m_ManifestConfig.m_Root / _Name, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);

			mint Length = File.f_GetLength();
			mint Start = _Random.f_GetValue(Length);
			mint End = Start + fg_Min(_Random.f_GetValue(Length), fg_Min(Length - Start, 8192));

			for (mint Position = Start; Position < End; ++Position)
			{
				uint8 Data;
				File.f_SetPosition(Position);
				File.f_Read(&Data, 1);

				Data = ~Data;
				File.f_SetPosition(Position);
				File.f_Write(&Data, 1);
			}
		}

		void f_RandomShrinkFile(CStr const &_Name, NMisc::CRandomShiftRNG &_Random)
		{
			DMibCloudBackupManagerDebugOut("RandomShrinkFile {}\n", _Name);
			CFile File;
			File.f_Open(m_BackupConfig.m_ManifestConfig.m_Root / _Name, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);

			mint Length = File.f_GetLength();
			mint NewLen = fg_Max(_Random.f_GetValue(Length), 1);
			File.f_SetLength(NewLen);
		}

		void f_AppendFile(CStr const &_Name, mint _Size)
		{
			DMibCloudBackupManagerDebugOut("Append file      {} {}\n", _Name, _Size);
			TCVector<uint8> Data;
			mint BufferSize = fg_AlignUp(_Size, 4);
			Data.f_SetLen(BufferSize);

			for (mint i = 0; i < BufferSize; i += 4)
				*((uint32 *)(Data.f_GetArray() + i)) = NMisc::fg_GetRandomUnsigned();

			Data.f_SetLen(_Size);

			CFile File;
			File.f_Open(m_BackupConfig.m_ManifestConfig.m_Root / _Name, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);
			File.f_SetPositionFromEnd(0);
			File.f_Write(Data.f_GetArray(), _Size);
		}

		CDirectoryManifest f_ReadManifest(CStr const &_Path)
		{
			CDirectoryManifest Manifest;

			TCBinaryStreamFile<> Stream;
			Stream.f_Open(_Path, EFileOpen_Read | EFileOpen_ShareAll);
			Stream >> Manifest;

			return Manifest;
		}

		TCSet<CStr> f_GetManifestFiles(CDirectoryManifest const &_Manifest, EFileAttrib _AttribsInclude, EFileAttrib _AttribsExclude)
		{
			TCSet<CStr> Files;
			for (auto &File : _Manifest.m_Files)
			{
				if (_AttribsInclude && !(File.m_Attributes & _AttribsInclude))
					continue;
				if (File.m_Attributes & _AttribsExclude)
					continue;
				Files[File.f_GetFileName()];
			}
			return Files;
		}

		CBackupManagerClient::CConfig m_BackupConfig;
		TCActor<CBackupManagerClient> m_BackupClient;
		TCDistributedActorInterfaceWithID<CDistributedAppInterfaceBackup> m_BackupInterface;
		TCSharedPointer<CTestState> m_pState = fg_Construct();
		NConcurrency::CActorSubscription m_ChangeSubscription;
	};

	void fp_DoGeneralTests()
	{

#if DTestBackupManagerEnableLogging
		fg_GetSys()->f_AddStdErrLogger();
#endif

		TCSet<CStr> BackupManagerPermissionsForTest = fg_CreateSet<CStr>("Backup/WriteSelf", "Backup/ReadAll");

		CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
		CStr RootDirectory = ProgramDirectory + "/BackupManagerTests";

		CProcessLaunch::fs_KillProcessesInDirectory("*", {}, RootDirectory, g_Timeout);
	
		if (CFile::fs_FileExists(RootDirectory))
			CFile::fs_DeleteDirectoryRecursive(RootDirectory);

		CFile::fs_CreateDirectory(RootDirectory);
	
		CTrustManagerTestHelper TrustManagerState;
		TCActor<CDistributedActorTrustManager> TrustManager = TrustManagerState.f_TrustManager("TestHelper");
		CStr TestHostID = TrustManager(&CDistributedActorTrustManager::f_GetHostID).f_CallSync(g_Timeout);
		CTrustedSubscriptionTestHelper Subscriptions{TrustManager};
	
		CDistributedActorTrustManager_Address ServerAddress;
		ServerAddress.m_URL = "wss://[UNIX(666):{}/controller.sock]/"_f << fg_GetSafeUnixSocketPath(RootDirectory);
		TrustManager(&CDistributedActorTrustManager::f_AddListen, ServerAddress).f_CallSync(g_Timeout);
	
		CDistributedApp_LaunchHelperDependencies Dependencies;
		Dependencies.m_Address = ServerAddress.m_URL;
		Dependencies.m_TrustManager = TrustManager;
		Dependencies.m_DistributionManager = TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager).f_CallSync(g_Timeout);
	
		NMib::NConcurrency::CDistributedActorSecurity Security;
		Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CBackupManager::mc_pDefaultNamespace);
		Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security).f_CallSync(g_Timeout);
	
		TCActor<CDistributedApp_LaunchHelper> LaunchHelper = fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, DTestBackupManagerEnableLogging);
		auto Cleanup = g_OnScopeExit > [&]
			{
				LaunchHelper->f_BlockDestroy();
			}
		;

		// Copy Cloud Client for debugging
		CStr CloudClientDirectory = RootDirectory + "/MalterlibCloud";
		CFile::fs_CreateDirectory(CloudClientDirectory);
		CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/MalterlibCloud", CloudClientDirectory, nullptr);

		// Copy BackupManagers to their directories
		mint nBackupManagers = 1;
		{
			TCActorResultVector<void> BackupManagerLaunchesResults;
			TCVector<TCActor<CSeparateThreadActor>> FileActors;
			for (mint i = 0; i < nBackupManagers; ++i)
			{
				auto &FileActor = FileActors.f_Insert() = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("File actor"));
				g_Dispatch(FileActor) > [=]
					{
						CStr BackupManagerName = "BackupManager{sf0,sl2}"_f << i;
						CStr BackupManagerDirectory = RootDirectory + "/" + BackupManagerName;
						CFile::fs_CreateDirectory(BackupManagerDirectory);
						//CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/BackupManager", BackupManagerDirectory, nullptr);
					}
					> BackupManagerLaunchesResults.f_AddResult()
				;
			}
			fg_CombineResults(BackupManagerLaunchesResults.f_GetResults().f_CallSync());
		}

		// Launch BackupManagers
		TCActorResultVector<CDistributedApp_LaunchInfo> BackupManagerLaunchesResults;
		TCVector<CDistributedApp_LaunchInfo> BackupManagerLaunches;
		
		auto fLaunchSecretManagers = [&]
			{
				BackupManagerLaunches.f_Clear();
				BackupManagerLaunchesResults = {};

				for (mint i = 0; i < nBackupManagers; ++i)
				{
					CStr BackupManagerName = "BackupManager{sf0,sl2}"_f << i;
					CStr BackupManagerDirectory = RootDirectory + "/" + BackupManagerName;
					LaunchHelper(&CDistributedApp_LaunchHelper::f_LaunchInProcess, BackupManagerName, BackupManagerDirectory, &fg_ConstructApp_BackupManager)
						> BackupManagerLaunchesResults.f_AddResult()
					;
				}
				for (auto &LaunchResult : BackupManagerLaunchesResults.f_GetResults().f_CallSync(g_Timeout))
					BackupManagerLaunches.f_Insert(fg_Move(*LaunchResult));
			}
		;
		fLaunchSecretManagers();

		auto HelperActor = fg_ConcurrentActor();
		CCurrentActorScope CurrentActor{HelperActor};
		
		// Setup trust for BackupManagers
		
		struct CBackupManagerInfo
		{
			CStr const &f_GetHostID() const
			{
				return TCMap<CStr, CBackupManagerInfo>::fs_GetKey(*this);
			}

			TCSharedPointer<TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface>> m_pTrustInterface;
			CDistributedActorTrustManager_Address m_Address;
		};

		TCSet<CStr> AllBackupManagerHosts;
		TCMap<CStr, CBackupManagerInfo> AllBackupManagers;
		auto fSetupListen = [&]
			{
				AllBackupManagerHosts.f_Clear();
				AllBackupManagers.f_Clear();
				TCActorResultVector<void> ListenResults;
				mint iBackupManager = 0;
				for (auto &BackupManager : BackupManagerLaunches)
				{
					CStr BackupManagerName = "BackupManager{sf0,sl2}"_f << iBackupManager;
					CStr BackupManagerDirectory = RootDirectory + "/" + BackupManagerName;
					
					AllBackupManagerHosts[BackupManager.m_HostID];
					auto &BackupManagerInfo = AllBackupManagers[BackupManager.m_HostID];
					BackupManagerInfo.m_pTrustInterface = BackupManager.m_pTrustInterface;
					BackupManagerInfo.m_Address.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/BackupManagerTest.sock"_f << BackupManagerDirectory);
					DMibCallActor(*BackupManager.m_pTrustInterface, CDistributedActorTrustManagerInterface::f_AddListen, BackupManagerInfo.m_Address) > ListenResults.f_AddResult();
					++iBackupManager;
				}
				fg_CombineResults(ListenResults.f_GetResults().f_CallSync(g_Timeout));
			}
		;
		fSetupListen();
		
		TCActorResultVector<void> SetupTrustResults;

		static auto constexpr c_WaitForSubscriptions = EDistributedActorTrustManagerOrderingFlag_WaitForSubscriptions;
		auto fPermissions = [](auto &&_HostID, auto &&_Permissions)
			{
				return CDistributedActorTrustManagerInterface::CChangeHostPermissions{_HostID, _Permissions, c_WaitForSubscriptions};
			}
		;

		for (auto &BackupManager : AllBackupManagers)
		{
			auto pBackupManagerTrust = BackupManager.m_pTrustInterface;
			auto &BackupManagerTrust = *pBackupManagerTrust;
			CStr BackupManagerHostID = BackupManager.f_GetHostID();
			auto TrustBackupManagers = AllBackupManagerHosts;
			TrustBackupManagers.f_Remove(BackupManagerHostID);

			DMibCallActor
				(
					TrustManager
					, CDistributedActorTrustManager::f_AllowHostsForNamespace
					, CBackupManager::mc_pDefaultNamespace
					, fg_CreateSet<CStr>(BackupManagerHostID)
				 	, c_WaitForSubscriptions
				)
				> SetupTrustResults.f_AddResult()
			;
			
			DMibCallActor
				(
					BackupManagerTrust
					, CDistributedActorTrustManagerInterface::f_AddHostPermissions
					, fPermissions(TestHostID, BackupManagerPermissionsForTest)
				)
				> SetupTrustResults.f_AddResult()
			;

			for (auto &BackupManagerInner : AllBackupManagers)
			{
				CStr BackupManagerHostIDInner = BackupManagerInner.f_GetHostID();
				if (BackupManagerHostIDInner == BackupManagerHostID)
					continue;
				
				auto pBackupManagerTrustInner = BackupManagerInner.m_pTrustInterface;
				
				TCContinuation<void> Continuation;
				DMibCallActor
					(
					 	BackupManagerTrust
					 	, CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket
					 	, CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{BackupManager.m_Address}
					)
					> Continuation / [=](CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket)
					{
						auto &BackupManagerTrustInner = *pBackupManagerTrustInner;
						DMibCallActor(BackupManagerTrustInner, CDistributedActorTrustManagerInterface::f_AddClientConnection, _Ticket.m_Ticket, g_Timeout, -1) > Continuation.f_ReceiveAny();
					}
				;
				Continuation.f_Dispatch() > SetupTrustResults.f_AddResult();

			}
		}
		
		SetupTrustResults.f_GetResults().f_CallSync(g_Timeout);

		{
			CTrustedSubscriptionTestHelper Subscriptions{TrustManager};
			auto BackupManagers = Subscriptions.f_SubscribeMultiple<CBackupManager>(nBackupManagers);
			auto BackupManager = BackupManagers[0];

			CStr TestBackupDirectory = RootDirectory + "/RecursiveBackupSource";

			if (CFile::fs_FileExists(TestBackupDirectory))
				CFile::fs_DeleteDirectoryRecursive(TestBackupDirectory);

			CFile::fs_CreateDirectory(TestBackupDirectory);

			CBackupClientHelper BackupHelper{TestBackupDirectory};

			BackupHelper.f_CreateFile("File1", 1024);
			BackupHelper.f_CreateFile("Dir1/File1", 1024);
			BackupHelper.f_CreateFile("Dir1/File2", 2048);
			BackupHelper.f_CreateFile("Dir2/File1", 1024);
			BackupHelper.f_CreateSymlink("Dir1/Symlink1", "File1", false, false);
			BackupHelper.f_CreateSymlink("Dir1/Symlink2", "FileInvalid1", false, false);
			BackupHelper.f_CreateSymlink("Dir1/SymlinkRel1", "File1", true, false);
			BackupHelper.f_CreateSymlink("Dir1/SymlinkRel2", "FileInvalid1", true, false);
			BackupHelper.f_CreateSymlink("Dir1/SymlinkDir1", "Dir2", false, true);
			BackupHelper.f_CreateSymlink("Dir1/SymlinkDir2", "Dir2Invalid1", false, true);
			BackupHelper.f_CreateSymlink("Dir1/SymlinkDirRel1", "Dir2", true, true);
			BackupHelper.f_CreateSymlink("Dir1/SymlinkDirRel2", "Dir2Invalid1", true, true);

			TCSet<CStr> DefaultSymlinks =
				{
					"Dir1/Symlink1"
					, "Dir1/Symlink2"
					, "Dir1/SymlinkDir1"
					, "Dir1/SymlinkDir2"
					, "Dir1/SymlinkDirRel1"
					, "Dir1/SymlinkDirRel2"
					, "Dir1/SymlinkRel1"
					, "Dir1/SymlinkRel2"
				}
			;

			TCSet<CStr> DefaultFiles = {"File1", "Dir1/File1", "Dir1/File2", "Dir2/File1"};

			COtherNotifications InitNotifications;
			TCSet<CStr> None;

			CStr BackupDestination = "{}/BackupManager00/Backups/{}-Test_{}"_f << RootDirectory << NProcess::NPlatform::fg_Process_GetComputerName() << TestHostID;
			CStr LatestDir = BackupDestination / "Latest";
			CStr ManifestPath = BackupDestination / "Manifest.bin";
			{
				DMibTestPath("General");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				{
					BackupHelper.f_Start(TrustManager, Dependencies);
				}
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(InitNotifications);

				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_FileFinished), ==, 4);
				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(InitNotifications.f_InitialAddedFiles(), ==, DefaultFiles);
				DMibExpect(InitNotifications.f_InitialDeletedFiles(), ==, None);
				DMibExpect(InitNotifications.f_InitialUpdatedFiles(), ==, None);

				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "File1"));
				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/File1"));
				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/File2"));
				DMibExpectTrue(!CFile::fs_FileExists(LatestDir / "Dir1/Symlink1"));
				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir2/File1"));

				DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "File1"));
				DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/File1"));
				DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/File2"));
				DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir2/File1"));

				auto Manifest = BackupHelper.f_ReadManifest(ManifestPath);

				DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_None, EFileAttrib_Link | EFileAttrib_Directory), ==, DefaultFiles);
				DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_Directory, EFileAttrib_Link), ==, TCSet<CStr>({"Dir1", "Dir2"}));
				DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_Link, EFileAttrib_None), ==, DefaultSymlinks);
			}
			{
				DMibTestPath("Include wildcards");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto &ManifestConfig = BackupHelper.m_BackupConfig.m_ManifestConfig;
				ManifestConfig.m_IncludeWildcards = {{"^File1", {}}};

				BackupHelper.f_Start(TrustManager, Dependencies);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(InitNotifications);

				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(InitNotifications.f_InitialAddedFiles(), ==, None);
				DMibExpect(InitNotifications.f_InitialDeletedFiles(), ==, TCSet<CStr>{"Dir1/File2"});
				DMibExpect(InitNotifications.f_InitialUpdatedFiles(), ==, None);

				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "File1"));
				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/File1"));
				DMibExpectFalse(CFile::fs_FileExists(LatestDir / "Dir1/File2"));
				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir2/File1"));
			}
			{
				DMibTestPath("Wildcard destinations");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto &ManifestConfig = BackupHelper.m_BackupConfig.m_ManifestConfig;
				ManifestConfig.m_IncludeWildcards = {{"^File1", "OverrideDir"}, {"Dir1/File2", "OverrideDir2"}};

				BackupHelper.f_Start(TrustManager, Dependencies);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(InitNotifications);

				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_FileFinished), ==, 4);
				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(InitNotifications.f_InitialAddedFiles(), ==, TCSet<CStr>({"OverrideDir/File1", "OverrideDir/Dir1/File1", "OverrideDir2/File2", "OverrideDir/Dir2/File1"}));
				DMibExpect(InitNotifications.f_InitialDeletedFiles(), ==, TCSet<CStr>({"File1", "Dir1/File1", "Dir2/File1"}));
				DMibExpect(InitNotifications.f_InitialUpdatedFiles(), ==, None);

				DMibExpectFalse(CFile::fs_FileExists(LatestDir / "Dir1"));
				DMibExpectFalse(CFile::fs_FileExists(LatestDir / "Dir2"));

				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "OverrideDir/File1"));
				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "OverrideDir/Dir1/File1"));
				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "OverrideDir2/File2"));
				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "OverrideDir/Dir2/File1"));

				BackupHelper.f_ModifyFile("Dir1/File2", 500);

				COtherNotifications ActionNotifications;
				COtherNotifications CleanupNotifications;

				auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

				DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
				DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

				DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
				DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, >, 0);
				DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, <, 1024);
				DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);

				DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "OverrideDir2/File2", "Dir1/File2"));
			}
			{
				DMibTestPath("Conflicting wildcard destinations");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto &ManifestConfig = BackupHelper.m_BackupConfig.m_ManifestConfig;
				ManifestConfig.m_IncludeWildcards = {{"^File1", "OverrideDir"}, {"Dir1/File1", "OverrideDir2"}};

				BackupHelper.f_Start(TrustManager, Dependencies);
				auto BackupError = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_BackupError>(InitNotifications);

				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);

				DMibExpect(BackupError.m_ErrorMessage, ==, "Failed to get manifest: Manifest config maps same file to different destinations");
				DMibExpectTrue(BackupError.m_bFatal);
			}
			{
				DMibTestPath("Exclude wildcards");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto &ManifestConfig = BackupHelper.m_BackupConfig.m_ManifestConfig;
				ManifestConfig.m_ExcludeWildcards = {"*File1"};

				BackupHelper.f_Start(TrustManager, Dependencies);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(InitNotifications);

				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_FileFinished), ==, 1);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(InitNotifications.f_InitialAddedFiles(), ==, TCSet<CStr>({"Dir1/File2"}));
				DMibExpect(InitNotifications.f_InitialDeletedFiles(), ==, TCSet<CStr>({"OverrideDir/File1", "OverrideDir/Dir1/File1", "OverrideDir/Dir2/File1", "OverrideDir2/File2"}));
				DMibExpect(InitNotifications.f_InitialUpdatedFiles(), ==, None);

				DMibExpectFalse(CFile::fs_FileExists(LatestDir / "File1"));
				DMibExpectFalse(CFile::fs_FileExists(LatestDir / "Dir1/File1"));
				DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/File2"));
				DMibExpectFalse(CFile::fs_FileExists(LatestDir / "Dir2/File1"));
			}
			{
				DMibTestPath("Append RSync");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				BackupHelper.f_Start(TrustManager, Dependencies);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(InitNotifications);

				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_FileFinished), ==, 3);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(InitNotifications.f_InitialAddedFiles(), ==, TCSet<CStr>({"File1", "Dir1/File1", "Dir2/File1"}));
				DMibExpect(InitNotifications.f_InitialDeletedFiles(), ==, None);
				DMibExpect(InitNotifications.f_InitialUpdatedFiles(), ==, None);

				BackupHelper.f_AppendFile("Dir1/File2", 553);

				COtherNotifications ActionNotifications;
				COtherNotifications CleanupNotifications;

				auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

				DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
				DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

				DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
				DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, >, 0);
				DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, <, 1024);
				DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);

				DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/File2"));
			}
			{
				DMibTestPath("Append semantics");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto &ManifestConfig = BackupHelper.m_BackupConfig.m_ManifestConfig;
				ManifestConfig.m_AddSyncFlagsWildcards["*/File2"] = EDirectoryManifestSyncFlag_Append;

				BackupHelper.f_Start(TrustManager, Dependencies);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(InitNotifications);

				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(InitNotifications.f_InitialAddedFiles(), ==, None);
				DMibExpect(InitNotifications.f_InitialDeletedFiles(), ==, None);
				DMibExpect(InitNotifications.f_InitialUpdatedFiles(), ==, TCSet<CStr>({"Dir1/File2"}));

				BackupHelper.f_AppendFile("Dir1/File2", 553);

				COtherNotifications ActionNotifications;
				COtherNotifications CleanupNotifications;

				auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

				DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
				DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

				DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Append);
				DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, ==, 0);
				DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, ==, 553);
				DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);

				DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/File2"));
			}
			{
				DMibTestPath("Remove wildcard flags");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto &ManifestConfig = BackupHelper.m_BackupConfig.m_ManifestConfig;
				ManifestConfig.m_AddSyncFlagsWildcards["*"] = EDirectoryManifestSyncFlag_Append;
				ManifestConfig.m_RemoveSyncFlagsWildcards["*/File2"] = EDirectoryManifestSyncFlag_Append;

				BackupHelper.f_Start(TrustManager, Dependencies);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(InitNotifications);
				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(InitNotifications.f_InitialAddedFiles(), ==, None);
				DMibExpect(InitNotifications.f_InitialDeletedFiles(), ==, None);
				DMibExpect(InitNotifications.f_InitialUpdatedFiles(), ==, TCSet<CStr>({"File1", "Dir1/File1", "Dir2/File1"}));

				BackupHelper.f_AppendFile("Dir1/File2", 553);

				COtherNotifications ActionNotifications;
				COtherNotifications CleanupNotifications;

				auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

				DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
				DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

				DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
				DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, >, 0);
				DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, <, 1024);
				DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);

				DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/File2"));
			}
			{
				DMibTestPath("Renames");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto &ManifestConfig = BackupHelper.m_BackupConfig.m_ManifestConfig;
				ManifestConfig.m_IncludeWildcards = {{"Dir1/^*"}};

				BackupHelper.f_Start(TrustManager, Dependencies);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(InitNotifications);
				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(InitNotifications.f_InitialAddedFiles(), ==, None);
				DMibExpect(InitNotifications.f_InitialDeletedFiles(), ==, TCSet<CStr>({"File1", "Dir2/File1"}));
				DMibExpect(InitNotifications.f_InitialUpdatedFiles(), ==, None);

				{
					DMibTestPath("Only sync one dir");
					DMibExpectFalse(CFile::fs_FileExists(LatestDir / "Dir2"));
					DMibExpectFalse(CFile::fs_FileExists(LatestDir / "File1"));
				}

				{
					DMibTestPath("Rename inside");
					BackupHelper.f_RenameFile("Dir1/File2", "Dir1/File3");

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Rename);
					DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, ==, 0);
					DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, ==, 0);
					DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);
					DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/File3"))(NTest::ETest_FailAndStop);
					DMibExpectFalse(CFile::fs_FileExists(LatestDir / "Dir1/File2"));
					DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/File3"));
				}
				{
					DMibTestPath("Rename out of");

					BackupHelper.f_RenameFile("Dir1/File3", "Dir2/File2");

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Delete);
					DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, ==, 0);
					DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, ==, 0);
					DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);
					DMibExpectFalse(CFile::fs_FileExists(LatestDir / "Dir1/File3"));
					DMibExpectFalse(CFile::fs_FileExists(LatestDir / "Dir2/File2"));
				}
				{
					DMibTestPath("Rename into");

					BackupHelper.f_RenameFile("Dir2/File2", "Dir1/File2");

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
					DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, >, 0);
					DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, >, 0);
					DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);
					DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/File2"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/File2"));
				}
				{
					DMibTestPath("Rename dir into");

					BackupHelper.f_RenameFile("Dir2", "Dir1/Dir2");

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
					DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, >, 0);
					DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, >, 0);
					DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);

					DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/Dir2/File1"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/Dir2/File1"));
				}
				{
					DMibTestPath("Rename dir inside");

					BackupHelper.f_RenameFile("Dir1/Dir2", "Dir1/Dir3");

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Rename);
					DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, ==, 0);
					DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, ==, 0);
					DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);

					DMibExpectTrue(!CFile::fs_FileExists(LatestDir / "Dir1/Dir2/File1"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/Dir3/File1"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/Dir3/File1"));
				}
				{
					DMibTestPath("Rename dir out");

					BackupHelper.f_RenameFile("Dir1/Dir3", "Dir2");

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Delete);
					DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, ==, 0);
					DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, ==, 0);
					DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);

					DMibExpectTrue(!CFile::fs_FileExists(LatestDir / "Dir1/Dir3/File1"))(NTest::ETest_FailAndStop);
				}
				{
					DMibTestPath("Rename dir");

					BackupHelper.f_RenameFile("Dir1", "Dir3");

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished0 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					auto FileFinished1 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished0.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Delete);
					DMibExpect(FileFinished0.m_TransferStats.m_IncomingBytes, ==, 0);
					DMibExpect(FileFinished0.m_TransferStats.m_OutgoingBytes, ==, 0);
					DMibExpect(FileFinished0.m_TransferStats.m_nSeconds, >, 0.0);
					DMibExpect(FileFinished1.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Delete);
					DMibExpect(FileFinished1.m_TransferStats.m_IncomingBytes, ==, 0);
					DMibExpect(FileFinished1.m_TransferStats.m_OutgoingBytes, ==, 0);
					DMibExpect(FileFinished1.m_TransferStats.m_nSeconds, >, 0.0);

					DMibExpectTrue(!CFile::fs_FileExists(LatestDir / "Dir1/File1"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(!CFile::fs_FileExists(LatestDir / "Dir1/File2"))(NTest::ETest_FailAndStop);
				}
				{
					DMibTestPath("Rename dir back");

					BackupHelper.f_RenameFile("Dir3", "Dir1");

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished0 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					auto FileFinished1 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished0.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
					DMibExpect(FileFinished0.m_TransferStats.m_IncomingBytes, >, 0);
					DMibExpect(FileFinished0.m_TransferStats.m_OutgoingBytes, >, 0);
					DMibExpect(FileFinished0.m_TransferStats.m_nSeconds, >, 0.0);
					DMibExpect(FileFinished1.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
					DMibExpect(FileFinished1.m_TransferStats.m_IncomingBytes, >, 0);
					DMibExpect(FileFinished1.m_TransferStats.m_OutgoingBytes, >, 0);
					DMibExpect(FileFinished1.m_TransferStats.m_nSeconds, >, 0.0);
					DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/File1"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/File1"));
					DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/File2"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/File2"));
				}
				{
					DMibTestPath("Switch file symlink");

					BackupHelper.f_RenameFile("Dir1/File1", "Dir1/Temp1");
					BackupHelper.f_RenameFile("Dir1/Symlink1", "Dir1/File1");
					BackupHelper.f_RenameFile("Dir1/Temp1", "Dir1/Symlink1");

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					if (FileFinished.m_TransferStats.m_Type == CBackupManagerClient::EFileTransferType_Delete)
						FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), >=, 1);
					DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Quiescent), >=, 0);
					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
					DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, >, 0);
					DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, >, 0);
					DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);

					auto Manifest = BackupHelper.f_ReadManifest(ManifestPath);
					TCSet<CStr> SwitchedFiles = DefaultFiles;
					TCSet<CStr> SwitchedSymlinks = DefaultSymlinks;

					SwitchedFiles.f_Remove("Dir1/File1");
					SwitchedFiles.f_Remove("Dir2/File1");
					SwitchedFiles.f_Remove("File1");

					SwitchedSymlinks.f_Remove("Dir1/Symlink1");

					SwitchedSymlinks["Dir1/File1"];
					SwitchedFiles["Dir1/Symlink1"];

					DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_None, EFileAttrib_Link | EFileAttrib_Directory), ==, SwitchedFiles);
					DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_Directory, EFileAttrib_Link), ==, TCSet<CStr>({"Dir1"}));
					DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_Link, EFileAttrib_None), ==, SwitchedSymlinks);

					DMibExpectTrue(!CFile::fs_FileExists(LatestDir / "Dir1/File1"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/Symlink1"))(NTest::ETest_FailAndStop);
				}
				{
					DMibTestPath("Switch file symlink back");

					BackupHelper.f_RenameFile("Dir1/File1", "Dir1/Temp1");
					BackupHelper.f_RenameFile("Dir1/Symlink1", "Dir1/File1");
					BackupHelper.f_RenameFile("Dir1/Temp1", "Dir1/Symlink1");

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					if (FileFinished.m_TransferStats.m_Type == CBackupManagerClient::EFileTransferType_Delete)
						FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), >=, 1);
					DMibExpect(ActionNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_Quiescent), >=, 0);
					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
					DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, >, 0);
					DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, >, 0);
					DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);

					auto Manifest = BackupHelper.f_ReadManifest(ManifestPath);
					TCSet<CStr> SwitchedFiles = DefaultFiles;

					SwitchedFiles.f_Remove("Dir2/File1");
					SwitchedFiles.f_Remove("File1");

					DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_None, EFileAttrib_Link | EFileAttrib_Directory), ==, SwitchedFiles);
					DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_Directory, EFileAttrib_Link), ==, TCSet<CStr>({"Dir1"}));
					DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_Link, EFileAttrib_None), ==, DefaultSymlinks);

					DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/File1"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(!CFile::fs_FileExists(LatestDir / "Dir1/Symlink1"))(NTest::ETest_FailAndStop);
				}
				{
					DMibTestPath("Delete watched dir");

					CStr Source = BackupHelper.f_GetPath("Dir1");
					CStr Destination = BackupHelper.f_GetPath("Dir3");

					CFile::fs_DiffCopyFileOrDirectory(Source, Destination, nullptr);
					CFile::fs_DeleteDirectoryRecursive(Source);

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished0 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					auto FileFinished1 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished0.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Delete);
					DMibExpect(FileFinished0.m_TransferStats.m_IncomingBytes, ==, 0);
					DMibExpect(FileFinished0.m_TransferStats.m_OutgoingBytes, ==, 0);
					DMibExpect(FileFinished0.m_TransferStats.m_nSeconds, >, 0.0);
					DMibExpect(FileFinished1.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Delete);
					DMibExpect(FileFinished1.m_TransferStats.m_IncomingBytes, ==, 0);
					DMibExpect(FileFinished1.m_TransferStats.m_OutgoingBytes, ==, 0);
					DMibExpect(FileFinished1.m_TransferStats.m_nSeconds, >, 0.0);

					DMibExpectTrue(!CFile::fs_FileExists(LatestDir / "Dir1/File1"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(!CFile::fs_FileExists(LatestDir / "Dir1/File2"))(NTest::ETest_FailAndStop);
				}
				{
					DMibTestPath("Rename new watched dir back");

					BackupHelper.f_RenameFile("Dir3", "Dir1");

					COtherNotifications ActionNotifications;
					COtherNotifications CleanupNotifications;

					auto FileFinished0 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					auto FileFinished1 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications);
					BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications);

					DMibExpect(ActionNotifications.f_UnclaimedNotificaions(), ==, None);
					DMibExpect(CleanupNotifications.f_UnclaimedNotificaions(), ==, None);

					DMibExpect(FileFinished0.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
					DMibExpect(FileFinished0.m_TransferStats.m_IncomingBytes, >, 0);
					DMibExpect(FileFinished0.m_TransferStats.m_OutgoingBytes, >, 0);
					DMibExpect(FileFinished0.m_TransferStats.m_nSeconds, >, 0.0);
					DMibExpect(FileFinished1.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
					DMibExpect(FileFinished1.m_TransferStats.m_IncomingBytes, >, 0);
					DMibExpect(FileFinished1.m_TransferStats.m_OutgoingBytes, >, 0);
					DMibExpect(FileFinished1.m_TransferStats.m_nSeconds, >, 0.0);
					DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/File1"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/File1"));
					DMibExpectTrue(CFile::fs_FileExists(LatestDir / "Dir1/File2"))(NTest::ETest_FailAndStop);
					DMibExpectTrue(BackupHelper.f_FileIsSame(LatestDir, "Dir1/File2"));
				}
			}
			{
				DMibTestPath("Delete all");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto &ManifestConfig = BackupHelper.m_BackupConfig.m_ManifestConfig;
				ManifestConfig.m_IncludeWildcards = {{"^*"}};

				BackupHelper.f_Start(TrustManager, Dependencies);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(InitNotifications);
				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_FileFinished), ==, 2);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(InitNotifications.f_InitialAddedFiles(), ==, TCSet<CStr>({"File1", "Dir2/File1"}));
				DMibExpect(InitNotifications.f_InitialDeletedFiles(), ==, None);
				DMibExpect(InitNotifications.f_InitialUpdatedFiles(), ==, None);

				BackupHelper.f_DeleteFile("Dir1/File1");

				COtherNotifications ActionNotifications0;
				COtherNotifications CleanupNotifications0;
				auto FileFinished0 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications0);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications0);
				DMibExpect(ActionNotifications0.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
				DMibExpect(ActionNotifications0.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(CleanupNotifications0.f_UnclaimedNotificaions(), ==, None);

				BackupHelper.f_DeleteFile("Dir1/File2");

				COtherNotifications ActionNotifications1;
				COtherNotifications CleanupNotifications1;
				auto FileFinished1 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications1);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications1);
				DMibExpect(ActionNotifications1.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
				DMibExpect(ActionNotifications1.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(CleanupNotifications1.f_UnclaimedNotificaions(), ==, None);

				BackupHelper.f_DeleteFile("File1");
				COtherNotifications ActionNotifications2;
				COtherNotifications CleanupNotifications2;
				auto FileFinished2 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications2);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications2);
				DMibExpect(ActionNotifications2.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
				DMibExpect(ActionNotifications2.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(CleanupNotifications2.f_UnclaimedNotificaions(), ==, None);

				BackupHelper.f_DeleteFile("Dir2/File1");
				COtherNotifications ActionNotifications3;
				COtherNotifications CleanupNotifications3;
				auto FileFinished3 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications3);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications3);
				DMibExpect(ActionNotifications3.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
				DMibExpect(ActionNotifications3.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(CleanupNotifications3.f_UnclaimedNotificaions(), ==, None);

				DMibExpect(FileFinished0.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Delete);
				DMibExpect(FileFinished1.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Delete);
				DMibExpect(FileFinished2.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Delete);
				DMibExpect(FileFinished3.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_Delete);

				auto FilesInLatest = CFile::fs_FindFiles(LatestDir / "*", EFileAttrib_Directory | EFileAttrib_File, true);
				DMibExpect(FilesInLatest, ==, TCVector<CStr>{});
			}
			{
				DMibTestPath("Adds");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto &ManifestConfig = BackupHelper.m_BackupConfig.m_ManifestConfig;
				ManifestConfig.m_IncludeWildcards = {{"Dir1/^*"}};

				BackupHelper.f_Start(TrustManager, Dependencies);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(InitNotifications);
				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(InitNotifications.f_InitialAddedFiles(), ==, None);
				DMibExpect(InitNotifications.f_InitialDeletedFiles(), ==, None);
				DMibExpect(InitNotifications.f_InitialUpdatedFiles(), ==, None);

				BackupHelper.f_CreateFile("Dir2/File1", 1024);
				BackupHelper.f_CreateFile("File1", 1024);

				BackupHelper.f_CreateFile("Dir1/File1", 1024);
				COtherNotifications ActionNotifications0;
				COtherNotifications CleanupNotifications0;
				auto FileFinished0 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications0);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications0);
				DMibExpect(ActionNotifications0.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
				DMibExpect(ActionNotifications0.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(CleanupNotifications0.f_UnclaimedNotificaions(), ==, None);

				BackupHelper.f_CreateFile("Dir1/File2", 2048);
				COtherNotifications ActionNotifications1;
				COtherNotifications CleanupNotifications1;
				auto FileFinished1 = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications1);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(CleanupNotifications1);
				DMibExpect(ActionNotifications1.f_ClaimNotifications(CBackupManagerClient::ENotification_Unquiescent), ==, 1);
				DMibExpect(ActionNotifications1.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(CleanupNotifications1.f_UnclaimedNotificaions(), ==, None);

				DMibExpect(FileFinished0.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
				DMibExpect(FileFinished0.m_TransferStats.m_IncomingBytes, >, 0);
				DMibExpect(FileFinished0.m_TransferStats.m_OutgoingBytes, >, 1024);
				DMibExpect(FileFinished0.m_TransferStats.m_nSeconds, >, 0.0);

				DMibExpect(FileFinished1.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
				DMibExpect(FileFinished1.m_TransferStats.m_IncomingBytes, >, 0);
				DMibExpect(FileFinished1.m_TransferStats.m_OutgoingBytes, >, 2048);
				DMibExpect(FileFinished1.m_TransferStats.m_nSeconds, >, 0.0);

				auto FilesInLatest = CFile::fs_FindFiles(LatestDir / "*", EFileAttrib_File, true);
				FilesInLatest.f_Sort();
				DMibExpect
					(
					 	FilesInLatest
					 	, ==
					 	, TCVector<CStr>
					 	(
						 	{
								LatestDir / "Dir1/File1"
								, LatestDir / "Dir1/File2"
							}
						)
					)
				;
			}
			{
				DMibTestPath("Dynamic manifest changes");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto &ManifestConfig = BackupHelper.m_BackupConfig.m_ManifestConfig;
				ManifestConfig.m_IncludeWildcards = {{"Dir2/^*"}};

				auto BackupFinishedSubscription = BackupHelper.f_Start(TrustManager, Dependencies);

				auto FileFinished = BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(InitNotifications);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);

				DMibExpect(FileFinished.m_TransferStats.m_Type, ==, CBackupManagerClient::EFileTransferType_RSync);
				DMibExpect(FileFinished.m_TransferStats.m_IncomingBytes, >, 0);
				DMibExpect(FileFinished.m_TransferStats.m_OutgoingBytes, >, 1024);
				DMibExpect(FileFinished.m_TransferStats.m_nSeconds, >, 0.0);

				NFile::CDirectoryManifestConfig ExtraManifestConfig;
				ExtraManifestConfig.m_IncludeWildcards = {{"Dir1/^*"}};
				DMibCallActor(BackupHelper.m_BackupInterface, CDistributedAppInterfaceBackup::f_AppendManifest, ExtraManifestConfig).f_CallSync(g_Timeout);

				BackupFinishedSubscription->f_Destroy().f_CallSync(g_Timeout);

				COtherNotifications AppendManifestNotifications;
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(AppendManifestNotifications);
				DMibExpect(AppendManifestNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(AppendManifestNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_FileFinished), ==, 2);
				DMibExpect(AppendManifestNotifications.f_UnclaimedNotificaions(), ==, None);
				DMibExpect(AppendManifestNotifications.f_InitialAddedFiles(), ==, TCSet<CStr>({"Dir2/File1"}));
				DMibExpect(AppendManifestNotifications.f_InitialDeletedFiles(), ==, None);
				DMibExpect(AppendManifestNotifications.f_InitialUpdatedFiles(), ==, TCSet<CStr>({"Dir1/File1", "Dir1/File2"}));

				auto FilesInLatest = CFile::fs_FindFiles(LatestDir / "*", EFileAttrib_File, true);
				FilesInLatest.f_Sort();
				DMibExpect
					(
					 	FilesInLatest
					 	, ==
					 	, TCVector<CStr>
					 	(
						 	{
								LatestDir / "Dir1/File1"
								, LatestDir / "Dir1/File2"
								, LatestDir / "Dir2/File1"
							}
						)
					)
				;
			}
			{
				DMibTestPath("Downloads");
				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto BackupSources = DMibCallActor(BackupManager, CBackupManager::f_ListBackupSources).f_CallSync(g_Timeout);

				DMibAssert(BackupSources.f_GetLen(), ==, 1);

				CStr TestDownloadDirectory = RootDirectory + "/TestDownload";

				NFile::CDirectorySyncReceive::CConfig Receive;
				Receive.m_BasePath = TestDownloadDirectory;

				CActorSubscription Subscription;
				auto DownloadResult = fg_DownloadBackup(BackupManager, BackupSources[0], CTime{}, fg_Move(Receive), Subscription).f_CallSync(g_Timeout);

				DMibExpectTrue(BackupHelper.f_FileIsSame(TestDownloadDirectory, "Dir1/File1"));
				DMibExpectTrue(BackupHelper.f_FileIsSame(TestDownloadDirectory, "Dir1/File2"));
				DMibExpectTrue(BackupHelper.f_FileIsSame(TestDownloadDirectory, "Dir2/File1"));

				TCSet<CStr> SourceFiles = {"Dir1/File1", "Dir1/File2", "Dir2/File1", "Dir1", "Dir2"};
				SourceFiles += DefaultSymlinks;
				TCSet<CStr> ManifestFiles;

				for (auto &File : DownloadResult.m_Manifest.m_Files)
					ManifestFiles[File.f_GetFileName()];

				DMibExpect(ManifestFiles, ==, SourceFiles);
			}
			{
				DMibTestPath("Cleanup");

				DMibExpect(CFile::fs_FindFiles(BackupDestination / "Sync_*").f_GetLen(), ==, 1);
				DMibExpect(CFile::fs_FindFiles(BackupDestination / "CheckedOut_*"), ==, TCVector<CStr>{});
			}
			{
				DMibTestPath("Change spree");

				// Test that backup is eventually consistent

				CBackupClientHelper BackupHelper{TestBackupDirectory};

				auto &ManifestConfig = BackupHelper.m_BackupConfig.m_ManifestConfig;
				ManifestConfig.m_AddSyncFlagsWildcards["Dir1/*"] = EDirectoryManifestSyncFlag_Append;

				BackupHelper.f_Start(TrustManager, Dependencies);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(InitNotifications);

				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_InitialFinished), ==, 1);
				DMibExpect(InitNotifications.f_ClaimNotifications(CBackupManagerClient::ENotification_FileFinished), ==, 1);
				DMibExpect(InitNotifications.f_UnclaimedNotificaions(), ==, None);

				DMibCloudBackupManagerDebugOut("-----------------------------------------\n-----------------------------------------\n-----------------------------------------\n");

				NMisc::CRandomShiftRNG Random;

				TCLinkedList<CStr> Files = {"Dir1/File1", "Dir1/File2", "Dir2/File1", "File1"};
				TCLinkedList<CStr> Symlinks;

				for (auto &Symlink : DefaultSymlinks)
					Symlinks.f_Insert(Symlink);

				mint iFileSequence = 3;

				TCLinkedList<CStr> Directories = {"", "Dir1", "Dir2"};

				auto fGetDirectory = [&]() -> CStr &
					{
						mint FileIndex = Random.f_GetValue(Directories.f_GetLen());
						auto iFile = Directories.f_GetIterator();
						for (; FileIndex; --FileIndex)
							++iFile;

						return *iFile;
					}
				;

				auto fGetFile = [&]() -> CStr &
					{
						mint FileIndex = Random.f_GetValue(Files.f_GetLen());
						auto iFile = Files.f_GetIterator();
						for (; FileIndex; --FileIndex)
							++iFile;

						return *iFile;
					}
				;

				auto fGetSymlink = [&]() -> CStr &
					{
						mint FileIndex = Random.f_GetValue(Symlinks.f_GetLen());
						auto iFile = Symlinks.f_GetIterator();
						for (; FileIndex; --FileIndex)
							++iFile;

						return *iFile;
					}
				;

				mint nRandomTests = 2000;

				for (mint i = 0; i < nRandomTests; ++i)
				{
					switch (Random.f_GetValue(26u))
					{
					case 0: // Create
					case 1:
						{
							CStr ToAdd = fGetDirectory() / ("File{}"_f << iFileSequence++);
 							BackupHelper.f_CreateFile(ToAdd, Random.f_GetValue(8192u) + 1);
							Files.f_Insert(ToAdd);
						}
						break;
					case 2: // Delete
						{
							if (Files.f_IsEmpty())
								continue;

							auto &File = fGetFile();
							BackupHelper.f_DeleteFile(File);
							Files.f_Remove(File);
						}
						break;
					case 24: // Create directory
						{
							CStr ToAdd = "Dir{}"_f << iFileSequence++;
 							BackupHelper.f_CreateDirectory(ToAdd);
							Directories.f_Insert(ToAdd);
						}
						break;
					case 25: // Rename directory
						{
							auto &OldName = fGetDirectory();
							if (OldName.f_IsEmpty())
								continue;
							CStr NewName = "Dir{}"_f << iFileSequence++;
							BackupHelper.f_RenameFile(OldName, NewName);

							CStr OldReplace = OldName + "/";
							CStr NewReplace = NewName + "/";

							Directories.f_Remove(OldName);
							Directories.f_Insert(NewName);

							for (auto &File : Files)
								File = File.f_Replace(OldReplace, NewReplace);
							for (auto &File : Symlinks)
								File = File.f_Replace(OldReplace, NewReplace);
						}
						break;
					case 3: // Modify
					case 4:
					case 5:
					case 6:
					case 13:
					case 14:
					case 15:
					case 16:
					case 17:
					case 18:
					case 19:
					case 20:
					case 21:
					case 22:
					case 23:
						{
							if (Files.f_IsEmpty())
								continue;

							auto File = fGetFile();
							if (File.f_StartsWith("Dir1/"))
							{
								auto Size = Random.f_GetValue(8192u);
								BackupHelper.f_AppendFile(File, Size);
							}
							else
								BackupHelper.f_RandomModifyFile(File, Random);
						}
						break;
					case 7: // Rename
						{
							if (Files.f_IsEmpty())
								continue;

							auto &OldName = fGetFile();
							CStr NewName = fGetDirectory() / ("File{}"_f << iFileSequence++);
							BackupHelper.f_RenameFile(OldName, NewName);

							Files.f_Remove(OldName);
							Files.f_Insert(NewName);
						}
						break;
					case 8: // Create symlink
					case 9:
						{
							break;
							CStr ToAdd = fGetDirectory() / ("Symlink{}"_f << iFileSequence++);
							bool bDirectory = Random.f_GetValue(1u);
 							BackupHelper.f_CreateSymlink(ToAdd, bDirectory ? "Dir1" : "File1", Random.f_GetValue(1u), bDirectory);
							Symlinks.f_Insert(ToAdd);
						}
						break;
					case 10: // Delete symlink
						{
							if (Symlinks.f_IsEmpty())
								continue;

							auto &Symlink = fGetSymlink();
							BackupHelper.f_DeleteFile(Symlink);
							Symlinks.f_Remove(Symlink);
						}
						break;
					case 11: // Swap symlinkiness
						{
							if (Symlinks.f_IsEmpty() || Files.f_IsEmpty())
								continue;

							auto TempSequence = iFileSequence++;
							auto &Symlink = fGetSymlink();
							auto &File = fGetFile();
							CStr TempName = "Temp{}"_f << TempSequence;
							BackupHelper.f_SwitchFile(Symlink, File, TempName);

							fg_Swap(File, Symlink);
						}
						break;
					case 12: // Shrink files
						{
							if (Files.f_IsEmpty())
								continue;

							auto File = fGetFile();
							if (File.f_StartsWith("Dir1/"))
							{
								auto Size = Random.f_GetValue(8192u);
								BackupHelper.f_AppendFile(File, Size);
							}
							else
								BackupHelper.f_RandomShrinkFile(File, Random);
						}
						break;
					}
				}

				g_Timeout = nRandomTests;

				COtherNotifications ActionNotifications;
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Unquiescent>(ActionNotifications);
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(ActionNotifications);

				// Wait for last file to be visibile
				BackupHelper.f_CreateFile("FileEnd", 1024);
				while (BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_FileFinished>(ActionNotifications).m_FileName != "FileEnd")
					;
				BackupHelper.f_WaitForNotification<CBackupManagerClient::ENotification_Quiescent>(ActionNotifications);

				TCSet<CStr> SourceFiles;
				TCSet<CStr> DestinationFiles;

				TCSet<CStr> SourceManifestLinks;
				TCSet<CStr> SourceManifestDirectories;
				TCSet<CStr> SourceManifestFiles;

				for (auto &File : CFile::fs_FindFilesEx(TestBackupDirectory / "*", EFileAttrib_File | EFileAttrib_Directory | EFileAttrib_Link, true, false))
				{
					auto RelativePath = File.m_Path.f_Extract(TestBackupDirectory.f_GetLen() + 1);
					if (File.m_Attribs & EFileAttrib_Link)
					{
						SourceManifestLinks[RelativePath];
						continue;
					}
					else if (File.m_Attribs & EFileAttrib_Directory)
					{
						SourceManifestDirectories[RelativePath];
						continue;
					}
					else
						SourceManifestFiles[RelativePath];

					SourceFiles[RelativePath];
				}
				for (auto &File : CFile::fs_FindFiles(LatestDir / "*", EFileAttrib_File, true))
					DestinationFiles[File.f_Extract(LatestDir.f_GetLen() + 1)];

				TCSet<CStr> SourceFilesPruned = SourceFiles;
				TCSet<CStr> DestinationFilesPruned = DestinationFiles;

				for (auto &File : SourceFiles)
					DestinationFilesPruned.f_Remove(File);
				for (auto &File : DestinationFiles)
					SourceFilesPruned.f_Remove(File);

				DMibAssert(DestinationFilesPruned, ==, SourceFilesPruned);

				for (auto &File : DestinationFiles)
					DMibTest(DMibExpr(File) && DMibExpr(BackupHelper.f_FileIsSame(LatestDir, File)))(NTest::ETestFlag_Aggregated);

				auto Manifest = BackupHelper.f_ReadManifest(ManifestPath);

				DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_None, EFileAttrib_Link | EFileAttrib_Directory), ==, SourceManifestFiles);
				DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_Directory, EFileAttrib_Link), ==, SourceManifestDirectories);
				DMibExpect(BackupHelper.f_GetManifestFiles(Manifest, EFileAttrib_Link, EFileAttrib_None), ==, SourceManifestLinks);

			}
		}
	}
};

DMibTestRegister(CBackupManager_Tests, Malterlib::Cloud);
