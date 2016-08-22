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
							
							auto &Application = *(mp_Applications[Name] = fg_Construct(Name, this));
							
							Application.m_Executable = ApplicationJSON["Executable"].f_String(); 
							Application.m_RunAsUser = ApplicationJSON["RunAsUser"].f_String(); 
							Application.m_RunAsGroup = ApplicationJSON["RunAsGroup"].f_String();
							for (auto &Parameter : ApplicationJSON["Parameters"].f_Array())
								Application.m_ExecutableParameters.f_Insert(Parameter.f_String());
							for (auto &File : ApplicationJSON["Files"].f_Array())
								Application.m_Files.f_Insert(File.f_String());
							Application.m_EncryptionStorage = ApplicationJSON["EncryptionStorage"].f_String();
						}					
					}
					if (auto pAllowedKeyManagers = mp_StateDatabase.m_Data.f_GetMember("AllowedKeyManagers"))
					{
						for (auto &Allowed : pAllowedKeyManagers->f_Array())
							mp_AllowedKeyManagers[Allowed.f_String()];
					}
				}
				catch (NException::CException const &_Exception)
				{
					return _Exception;
				}
				return fg_Explicit();
			}
			
			void CAppManagerActor::fp_KeyManagerAvailable()
			{
				if (!mp_bAppsEncryptedLaunched)
				{
					mp_bAppsEncryptedLaunched = true;
					fp_LaunchEncryptedApps();
				}
			}

			TCContinuation<void> CAppManagerActor::fp_StartApp()
			{
				mp_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("App manager file operations")); 
				
				TCContinuation<void> Continuation;
				fg_ThisActor(this)(&CAppManagerActor::fp_ReadState) > Continuation / [this, Continuation]()
					{
						fp_LaunchNormalApps();
						mp_DistributionManager
							(
								&CActorDistributionManager::f_SubscribeActors
								, fg_CreateVector<CStr>("MalterlibCloudKeyManager")
								, fg_ThisActor(this)
								, [this](CAbstractDistributedActor &&_NewActor)
								{
									CStr HostID = _NewActor.f_GetRealHostID();
									
									auto Manager = _NewActor.f_GetActor<CKeyManager>();
									mp_KeyManagerToHost[Manager] = HostID;
									mp_HostToKeyManager[HostID] = Manager;
									
									if (!mp_AllowedKeyManagers.f_FindEqual(HostID))
									{
										DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Fraudulent key manager detected: '{}'", HostID);
										return;
									}
									mp_KeyManagers[Manager];
									fp_KeyManagerAvailable();
								}
								, [this](TCWeakDistributedActor<CActor> const &_RemovedActor)
								{
									if (auto *pHostID = mp_KeyManagerToHost.f_FindEqual(_RemovedActor))
									{
										mp_HostToKeyManager.f_Remove(*pHostID);
										mp_KeyManagerToHost.f_Remove(pHostID);
									}
									mp_KeyManagers.f_Remove(_RemovedActor);
								}
							)
							> Continuation / [this, Continuation](CActorSubscription &&_Callback)
							{
								mp_KeyManagerSubscription = fg_Move(_Callback);
								Continuation.f_SetResult();
							}
						;
					}
				;
				return Continuation;
			}
			
			TCContinuation<void> CAppManagerActor::fp_StopApp()
			{	
				TCContinuation<void> Continuation;

				TCActorResultVector<uint32> ApplicationStops;
				for (auto &pApplication : mp_Applications)
					pApplication->f_Stop(true) > ApplicationStops.f_AddResult();

				ApplicationStops.f_GetResults() > Continuation / [this, Continuation](TCVector<TCAsyncResult<uint32>> &&_Results)
					{
						for (auto &pApplication : mp_Applications)
						{
							pApplication->f_AbortPendingLaunches();
							pApplication->f_Clear();
						}
						
						Continuation.f_SetResult();
					}
				;

				return Continuation;
			}
		}		
	}
}
