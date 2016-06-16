// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/Process/ProcessLaunchActor>

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
					CApplication(CStr const &_Name)
						: m_Name(_Name)
					{
					}

					void f_Clear();
					void f_Delete();
					
					TCDispatchedActorCall<uint32> f_Stop();
					
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
					
					CStr m_LaunchState;
					CStr m_LastStdErr;
					CStr m_LastError;
					
					TCActor<CProcessLaunchActor> m_ProcessLaunch;
					CActorCallback m_ProcessLaunchSubscription;
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
				
				TCContinuation<void> fp_StartApp() override;
				TCContinuation<void> fp_StopApp() override;
				TCContinuation<void> fp_ReadState();
				void fp_LaunchApps();
				void fp_LaunchApp(TCSharedPointer<CApplication> const &_pApplication);
				void fp_ScheduleRelaunchApp(TCSharedPointer<CApplication> const &_pApplication);
				
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_EnumApplications(const NEncoding::CEJSON &_Params);
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_AddApplication(const NEncoding::CEJSON &_Params);
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_RemoveApplication(const NEncoding::CEJSON &_Params);
				TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_UpdateApplication(const NEncoding::CEJSON &_Params);
				
				static void fsp_UpdateAttributes(CStr const &_File);
				static CStr fsp_UnpackApplication(CStr const &_Source, CStr const &_Destination, TCSharedPointer<CApplication> const &_pApplication, TCVector<CStr> &o_Files);
				
				void fp_OutputApplicationStop(TCAsyncResult<uint32> const &_Result, CDistributedAppCommandLineResults &o_Results, CStr const &_Name);

				
				TCMap<CStr, TCSharedPointer<CApplication>> mp_Applications;
				
				TCActor<CSeparateThreadActor> mp_FileActor;
			};
		}		
	}
}
