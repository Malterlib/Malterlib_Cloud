// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Cloud/KeyManager>
#include <Mib/Cloud/VersionManager>

namespace NMib
{
	namespace NCloud
	{
		namespace NAppManager
		{
			struct CAppManagerActor : public NConcurrency::CDistributedAppActor
			{
				CAppManagerActor();
				
			private:
				struct CApplication : public TCSharedPointerIntrusiveBase<>
				{
					CApplication(CStr const &_Name, CAppManagerActor *_pThis)
						: m_Name(_Name)
						, m_pThis(_pThis)
					{
					}

					void f_Clear();
					void f_Delete();
					void f_AbortPendingLaunches();
					
					TCDispatchedActorCall<uint32> f_Stop(bool _bCloseEncryption);
					TCDispatchedActorCall<uint32> f_CloseEncryption(uint32 _Status);
					
					CStr f_GetDirectory();
					
					COnScopeExitShared f_SetInProgress();

					CStr const m_Name;
					CStr m_Executable; 
					CStr m_RunAsUser; 
					CStr m_RunAsGroup; 
					CStr m_EncryptionStorage;
					TCVector<CStr> m_ExecutableParameters;
					TCVector<CStr> m_Files;

					CStr m_VersionManagerApplication;
					CVersionManager::CVersionIdentifier m_LastInstalledVersion;
					CVersionManager::CVersionInformation m_LastInstalledVersionInfo;
					
					bool m_bDeleted = false;
					bool m_bStopped = false;
					bool m_bOperationInProgress = false;
					bool m_bEncryptionOpened = false;
					bool m_bLaunching = false;
					
					TCLinkedList<TCFunction <void (bool _bAborted)>> m_OnLaunchFinished;
					
					CStr m_LaunchState;
					CStr m_LastStdErr;
					CStr m_LastError;
					
					TCActor<CProcessLaunchActor> m_ProcessLaunch;
					CActorSubscription m_ProcessLaunchSubscription;
					
					CAppManagerActor *m_pThis;
				};
				
				struct CBashScriptOutput
				{
					CStr m_StdOut;
					CStr m_StdErr;
					uint32 m_Status = 1;
				};

				
				struct CVersionManagerApplication;
				struct CVersionManagerState;
				
				struct CVersionManagerVersion
				{
					struct CCompareApplicationByTime
					{
						bool operator()(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const;
					};
					
					struct CCompareApplication
					{
						bool operator()(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const;
						bool operator()(CVersionManager::CVersionIdentifier const &_Left, CVersionManagerVersion const &_Right) const;
						bool operator()(CVersionManagerVersion const &_Left, CVersionManager::CVersionIdentifier const &_Right) const;
					};

					CVersionManagerVersion(CVersionManagerState *_pVersionManager);
					~CVersionManagerVersion();
					void f_SetApplication(CVersionManagerApplication *_pApplication);
					CVersionManager::CVersionIdentifier const &f_GetVersionID() const;
					
					CVersionManagerState *m_pVersionManager;
					CVersionManagerApplication *m_pApplication = nullptr;
					CVersionManager::CVersionInformation m_VersionInfo;
					DIntrusiveLink(CVersionManagerVersion, TCAVLLink<>, m_ApplicationTimeLink);
					DIntrusiveLink(CVersionManagerVersion, TCAVLLink<>, m_ApplicationLink);
				};
				
				struct CVersionManagerApplication
				{
					CVersionManagerApplication(CAppManagerActor &_This);
					CStr const &f_GetApplicationName() const
					{
						return TCMap<CStr, CVersionManagerApplication>::fs_GetKey(*this);
					}
					
					TCAVLTree<CVersionManagerVersion::CLinkTraits_m_ApplicationTimeLink, CVersionManagerVersion::CCompareApplicationByTime> m_VersionsByTime;
					TCAVLTree<CVersionManagerVersion::CLinkTraits_m_ApplicationLink, CVersionManagerVersion::CCompareApplication> m_Versions;
					CAppManagerActor &m_This;
				};
				
				struct CVersionManagerState
				{
					TCDistributedActor<CVersionManager> const &f_GetManager() const
					{
						return TCMap<TCDistributedActor<CVersionManager>, CVersionManagerState>::fs_GetKey(*this); 
					}
					
					TCMap<CStr, TCMap<CVersionManager::CVersionIdentifier, CVersionManagerVersion>> m_Versions;
					
					CTrustedActorInfo m_HostInfo;
					CActorSubscription m_Subscription;
				};
				
				struct CVersionManagerDownloadState
				{
					TCActor<CFileTransferReceive> m_DownloadVersionReceive;
					CActorSubscription m_Subscription;
				};
				
				enum EEncryptOperation
				{
					EEncryptOperation_Setup
					, EEncryptOperation_Open
					, EEncryptOperation_Close
				};

				void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
				
				static CStr fsp_RunTool
					(
						CStr const &_Description
						, CStr const &_Tool
						, CStr const &_WorkingDir
						, TCVector<CStr> const &_Arguments
						, CStr const &_Home
						, CStr const &_User
						, TCMap<CStr, CStr> const &_Environment = fg_Default()
						, bool _bQuiet = false
					)
				;
				TCContinuation<CBashScriptOutput> fp_RunBashScript
					(
						CStr const &_Script
						, CStr const &_Description
						, CStr const &_Home
						, CStr const &_User
						, TCMap<CStr, CStr> const &_Environment
						, TCFunction<void (NMib::NStr::CStr const &_Output, TCActor<CProcessLaunchActor> const &_LaunchActor)> const &_fOnStdOutput
					)
				;
				
				TCContinuation<void> fp_StartApp(NEncoding::CEJSON const &_Params) override;
				TCContinuation<void> fp_StopApp() override;
				TCContinuation<void> fp_ReadState();
				void fp_LaunchNormalApps();
				void fp_LaunchEncryptedApps();
				TCContinuation<void> fp_LaunchApp(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption);
				TCContinuation<void> fp_LaunchAppInternal(TCSharedPointer<CApplication> const &_pApplication, bool _bOpenEncryption);
				void fp_ScheduleRelaunchApp(TCSharedPointer<CApplication> const &_pApplication);
				
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_EnumApplications(CEJSON const &_Params);
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_AddApplication(CEJSON const &_Params);
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_RemoveApplication(CEJSON const &_Params);
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_UpdateApplication(CEJSON const &_Params);
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_StartApplication(CEJSON const &_Params);
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_StopApplication(CEJSON const &_Params);
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_RestartApplication(CEJSON const &_Params);
				
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_ListAvailableVersions(CEJSON const &_Params);
				
				static void fsp_UpdateAttributes(CStr const &_File);
				static CStr fsp_UnpackApplication
					(
						CStr const &_Source
						, CStr const &_Destination
						, TCSharedPointer<CApplication> const &_pApplication
						, TCVector<CStr> &o_Files
						, TCSet<CStr> const &_AllowExist
					)
				;
				TCContinuation<void> fp_ChangeEncryption(TCSharedPointer<CApplication> const &_pApplication, EEncryptOperation _Operation, bool _bForceOverwrite);
				
				void fp_OutputApplicationStop(TCAsyncResult<uint32> const &_Result, CDistributedAppCommandLineResults &o_Results, CStr const &_Name);
				void fp_KeyManagerAvailable();
				
				void fp_VersionManagerAdded(TCDistributedActor<CVersionManager> const &_VersionManager, CTrustedActorInfo const &_Info);
				void fp_VersionManagerRemoved(TCWeakDistributedActor<CActor> const &_VersionManager);
				
				TCContinuation<CVersionManager::CVersionInformation> fp_DownloadApplicationFromManager
					(
						TCDistributedActor<CVersionManager> const &_Manager
						, CStr const &_ApplicationName
						, CVersionManager::CVersionIdentifier const &_VersionID
						, CStr const &_DestinationDir
					)
				;
				
				TCContinuation<CVersionManager::CVersionInformation> fp_DownloadApplication
					(
						CStr const &_ApplicationName
						, CVersionManager::CVersionIdentifier const &_VersionID
						, CStr const &_DestinationDir
					)
				;

				TCMap<CStr, TCSharedPointer<CApplication>> mp_Applications;
				TCActor<CSeparateThreadActor> mp_FileActor;
				TCTrustedActorSubscription<CKeyManager> mp_KeyManagerSubscription;
				TCTrustedActorSubscription<CVersionManager> mp_VersionManagerSubscription;
				bool mp_bAppsEncryptedLaunched = false;
				
				TCLinkedList<CVersionManagerDownloadState> mp_Downloads;
				
				TCMap<CStr, CVersionManagerApplication> mp_VersionManagerApplications;
				TCMap<TCDistributedActor<CVersionManager>, CVersionManagerState> mp_VersionManagers; 
			};
		}		
	}
}
