// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/Actor/Timer>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NAppManager
		{
			CAppManagerActor::CAppManagerActor()
				: CDistributedAppActor(CDistributedAppActor_Settings("AppManager", false))
			{
			}
			
			TCContinuation<void> CAppManagerActor::fp_ReadState()
			{
				try
				{
					if (auto pApplication = mp_StateDatabase.m_Data.f_GetMember("Applications"))
					{
						for (auto &ApplicationEntry : pApplication->f_Object())
						{
							CStr const &Name = ApplicationEntry.f_Name();
							auto &ApplicationJSON = ApplicationEntry.f_Value(); 
							
							auto &Application = *(mp_Applications[Name] = fg_Construct(Name));
							
							Application.m_Executable = ApplicationJSON["Executable"].f_String(); 
							Application.m_RunAsUser = ApplicationJSON["RunAsUser"].f_String(); 
							Application.m_RunAsGroup = ApplicationJSON["RunAsGroup"].f_String();
							for (auto &Parameter : ApplicationJSON["Parameters"].f_Array())
								Application.m_ExecutableParameters.f_Insert(Parameter.f_String());
							for (auto &File : ApplicationJSON["Files"].f_Array())
								Application.m_Files.f_Insert(File.f_String());
							Application.m_EncryptionStorage = ApplicationJSON["EncryptionStorage"].f_String();;
						}					
					}
				}
				catch (NException::CException const &_Exception)
				{
					return _Exception;
				}
				return fg_Explicit();
			}
			
			TCContinuation<void> CAppManagerActor::fp_StartApp()
			{
				mp_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("App manager file operations")); 
				
				TCContinuation<void> Continuation;
				fg_ThisActor(this)(&CAppManagerActor::fp_ReadState) > Continuation / [this, Continuation]()
					{
						Continuation.f_SetResult();
						fp_LaunchApps();
					}
				;
				return Continuation;
			}
			
			void CAppManagerActor::CApplication::f_Clear()
			{
				m_ProcessLaunch.f_Clear();
				m_ProcessLaunchSubscription.f_Clear();
			}
			
			void CAppManagerActor::CApplication::f_Delete()
			{
				f_Clear();
				m_bDeleted = true;
			}

			COnScopeExitShared CAppManagerActor::CApplication::f_SetInProgress()
			{
				DRequire(!m_bOperationInProgress);
				m_bOperationInProgress = true;
				return fg_OnScopeExitShared
					(
						[pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]
						{
							DCheck(pApplication->m_bOperationInProgress);
							pApplication->m_bOperationInProgress = false;
						}
					)
				;
			}
			
			TCDispatchedActorCall<uint32> CAppManagerActor::CApplication::f_Stop()
			{
				return fg_Dispatch
					(
						[pApplication = TCSharedPointer<CApplication>(fg_Explicit(this))]() -> TCContinuation<uint32>
						{
							if (!pApplication->m_ProcessLaunch)
								return fg_Explicit(0);
							if (pApplication->m_bStopped)
								return fg_Explicit(0);
							if (pApplication->m_bDeleted)
								return fg_Explicit(0);
							pApplication->m_bStopped = true;
							TCContinuation<uint32> Continuation;
							pApplication->m_ProcessLaunch(&CProcessLaunchActor::f_StopProcess) > [Continuation, pApplication](TCAsyncResult<uint32> &&_Result)
								{
									if (!_Result)
										DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Error stopping application '{}'", _Result.f_GetExceptionStr());
									else if (*_Result)
										DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Application '{}' exited with non 0 status: {}", pApplication->m_Name, *_Result);
									else
										DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Application '{}' exited cleanly", pApplication->m_Name);
									
									Continuation.f_SetResult(fg_Move(_Result));
								}
							;
							
							return Continuation;
						}
					)
				;
				
			}
			
			CStr CAppManagerActor::CApplication::f_GetDirectory()
			{
				return fg_Format("{}/App/{}", CFile::fs_GetProgramDirectory(), m_Name);
			}
			
			TCContinuation<void> CAppManagerActor::fp_StopApp()
			{	
				TCContinuation<void> Continuation;

				TCActorResultVector<uint32> ApplicationStops;
				for (auto &pApplication : mp_Applications)
					pApplication->f_Stop() > ApplicationStops.f_AddResult();

				ApplicationStops.f_GetResults() > Continuation / [this, Continuation](TCVector<TCAsyncResult<uint32>> &&_Results)
					{
						for (auto &pApplication : mp_Applications)
							pApplication->f_Clear();
						
						Continuation.f_SetResult();
					}
				;

				return Continuation;
			}
		}		
	}
}
