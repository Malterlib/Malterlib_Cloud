// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Cloud/KeyManager>

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
				
				TCContinuation<void> fp_StartApp() override;
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
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_AddAllowedKeyManager(CEJSON const &_Params);
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_RemoveAllowedKeyManager(CEJSON const &_Params);
				
				static void fsp_UpdateAttributes(CStr const &_File);
				static CStr fsp_UnpackApplication(CStr const &_Source, CStr const &_Destination, TCSharedPointer<CApplication> const &_pApplication, TCVector<CStr> &o_Files);
				TCContinuation<void> fp_ChangeEncryption(TCSharedPointer<CApplication> const &_pApplication, EEncryptOperation _Operation, bool _bForceOverwrite);
				
				void fp_OutputApplicationStop(TCAsyncResult<uint32> const &_Result, CDistributedAppCommandLineResults &o_Results, CStr const &_Name);
				void fp_KeyManagerAvailable();

				
				TCMap<CStr, TCSharedPointer<CApplication>> mp_Applications;
				
				TCActor<CSeparateThreadActor> mp_FileActor;
				
				TCSet<CStr> mp_AllowedKeyManagers;
				
				CActorSubscription mp_KeyManagerSubscription;
				TCMap<TCDistributedActor<CKeyManager>, CStr> mp_KeyManagerToHost;
				TCMap<CStr, TCDistributedActor<CKeyManager>> mp_HostToKeyManager;
				TCSet<TCDistributedActor<CKeyManager>> mp_KeyManagers;
				
				bool mp_bAppsEncryptedLaunched = false;
			};
		}		
	}
}
