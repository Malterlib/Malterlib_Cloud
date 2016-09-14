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
					if (auto pApplication = mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
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
							if (auto pValue = ApplicationJSON.f_GetMember("VersionManagerApplication", EJSONType_String))
								Application.m_VersionManagerApplication = pValue->f_String();
							if (auto pValue = ApplicationJSON.f_GetMember("LastInstalledVersion", EJSONType_Object))
								Application.m_LastInstalledVersion = CVersionManager::CVersionIdentifier::fs_FromJSON(*pValue);
							if (auto pValue = ApplicationJSON.f_GetMember("LastInstalledVersionInfo", EJSONType_Object))
								Application.m_LastInstalledVersionInfo = CVersionManager::CVersionInformation::fs_FromJSON(*pValue);
							
						}					
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

			TCContinuation<void> CAppManagerActor::fp_StartApp(NEncoding::CEJSON const &_Params)
			{
				mp_FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("App manager file operations")); 
				
				TCContinuation<void> Continuation;
				fg_ThisActor(this)(&CAppManagerActor::fp_ReadState) > Continuation / [this, Continuation]()
					{
						fp_LaunchNormalApps();
						mp_State.m_TrustManager
							(
								&CDistributedActorTrustManager::f_SubscribeTrustedActors<CKeyManager>
								, "com.malterlib/Cloud/KeyManager"
								, fg_ThisActor(this)
							)
							+ mp_State.m_TrustManager
							(
								&CDistributedActorTrustManager::f_SubscribeTrustedActors<CVersionManager>
								, "com.malterlib/Cloud/VersionManager"
								, fg_ThisActor(this)
							)
							> Continuation / [this, Continuation](TCTrustedActorSubscription<CKeyManager> &&_KeySubscrption, TCTrustedActorSubscription<CVersionManager> &&_VersionSubscrption)
							{
								mp_KeyManagerSubscription = fg_Move(_KeySubscrption);

								if (!mp_KeyManagerSubscription.m_Actors.f_IsEmpty())
									fp_KeyManagerAvailable();
								
								mp_KeyManagerSubscription.f_OnNewActor
									(
										[this](TCDistributedActor<CKeyManager> const &_KeyManager, CTrustedActorInfo const &_ActorInfo)
										{
											fp_KeyManagerAvailable();
										}
									)
								;

								mp_VersionManagerSubscription = fg_Move(_VersionSubscrption);

								if (!mp_VersionManagerSubscription.m_Actors.f_IsEmpty())
								{
									for (auto &TrustedActor : mp_VersionManagerSubscription.m_Actors)
										fp_VersionManagerAdded(TrustedActor.m_Actor, TrustedActor.m_TrustInfo);
								}
								
								mp_VersionManagerSubscription.f_OnNewActor
									(
										[this](TCDistributedActor<CVersionManager> const &_VersionManager, CTrustedActorInfo const &_ActorInfo)
										{
											fp_VersionManagerAdded(_VersionManager, _ActorInfo);
										}
									)
								;
								
								mp_VersionManagerSubscription.f_OnRemoveActor
									(
										[this](TCWeakDistributedActor<CActor> const &_VersionManagero)
										{
											fp_VersionManagerRemoved(_VersionManagero);
										}
									)
								;
								
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
