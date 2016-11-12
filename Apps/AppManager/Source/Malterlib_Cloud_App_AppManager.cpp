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
			
			void CAppManagerActor::fp_ApplicationCreated(TCSharedPointer<CApplication> const &_pApplication)
			{
				if (_pApplication->f_IsChildApp())
					return; // Parent cannot have parents
				auto &NewApplication = *_pApplication;
				for (auto &pApplication : mp_Applications)
				{
					auto &Application = *pApplication;
					if (Application.m_Settings.m_ParentApplication != NewApplication.m_Name)
						continue;
					Application.m_pParentApplication = &NewApplication;
					NewApplication.m_Children.f_Insert(Application);
				}
			}
			
			void CAppManagerActor::fp_InitApplications()
			{
				for (auto &pApplication : mp_Applications)
				{
					auto &Application = *pApplication;
					if (!Application.m_LastInstalledVersion.m_Platform.f_IsEmpty())
						mp_KnownPlatforms[Application.m_LastInstalledVersion.m_Platform];
					if (Application.f_IsChildApp())
					{
						auto *pParentApplication = mp_Applications.f_FindEqual(Application.m_Settings.m_ParentApplication);
						if (pParentApplication && !(*pParentApplication)->f_IsChildApp())
						{
							Application.m_pParentApplication = &**pParentApplication;
							Application.m_pParentApplication->m_Children.f_Insert(Application);
						}
					}
				}
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
							
							auto &Settings = Application.m_Settings; 
							
							Settings.m_Executable = ApplicationJSON["Executable"].f_String(); 
							Settings.m_RunAsUser = ApplicationJSON["RunAsUser"].f_String(); 
							Settings.m_RunAsGroup = ApplicationJSON["RunAsGroup"].f_String();
							for (auto &Parameter : ApplicationJSON["Parameters"].f_Array())
								Settings.m_ExecutableParameters.f_Insert(Parameter.f_String());
							for (auto &File : ApplicationJSON["Files"].f_Array())
								Application.m_Files.f_Insert(File.f_String());
							Settings.m_EncryptionStorage = ApplicationJSON["EncryptionStorage"].f_String();
							if (auto pValue = ApplicationJSON.f_GetMember("EncryptionFileSystem", EJSONType_String))
								Settings.m_EncryptionFileSystem = pValue->f_String();
							else if (!Settings.m_EncryptionStorage.f_IsEmpty())
								Settings.m_EncryptionFileSystem = "zfs";
							if (auto pValue = ApplicationJSON.f_GetMember("ParentApplication", EJSONType_String))
								Settings.m_ParentApplication = pValue->f_String();
							if (auto pValue = ApplicationJSON.f_GetMember("VersionManagerApplication", EJSONType_String))
								Settings.m_VersionManagerApplication = pValue->f_String();
							if (auto pValue = ApplicationJSON.f_GetMember("LastInstalledVersion", EJSONType_Object))
								Application.m_LastInstalledVersion = CVersionManager::CVersionIDAndPlatform::fs_FromJSON(*pValue);
							if (auto pValue = ApplicationJSON.f_GetMember("LastInstalledVersionInfo", EJSONType_Object))
								Application.m_LastInstalledVersionInfo = CVersionManager::CVersionInformation::fs_FromJSON(*pValue);
							if (auto pValue = ApplicationJSON.f_GetMember("AutoUpdate", EJSONType_Boolean))
								Settings.m_bAutoUpdate = pValue->f_Boolean();
							if (auto pValue = ApplicationJSON.f_GetMember("AutoUpdateTags", EJSONType_Array))
							{
								for (auto &Tag : pValue->f_Array())
									Settings.m_AutoUpdateTags[Tag.f_String()];
							}
							if (auto pValue = ApplicationJSON.f_GetMember("AutoUpdateBranches", EJSONType_Array))
							{
								for (auto &Tag : pValue->f_Array())
									Settings.m_AutoUpdateBranches[Tag.f_String()];
							}
							if (auto pValue = ApplicationJSON.f_GetMember("UpdateScripts", EJSONType_Object))
							{
								Settings.m_UpdateScripts.m_PreUpdate = (*pValue)["PreUpdate"].f_String();
								Settings.m_UpdateScripts.m_PostUpdate = (*pValue)["PostUpdate"].f_String();
								Settings.m_UpdateScripts.m_PostLaunch = (*pValue)["PostLaunch"].f_String();
								Settings.m_UpdateScripts.m_OnError = (*pValue)["OnError"].f_String();
							}
							if (auto pValue = ApplicationJSON.f_GetMember("SelfUpdateSource", EJSONType_Boolean))
								Settings.m_bSelfUpdateSource = pValue->f_Boolean();
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
				mp_KnownPlatforms[DMalterlibCloudPlatform];
				
				TCContinuation<void> Continuation;
				fg_ThisActor(this)(&CAppManagerActor::fp_ReadState) > Continuation / [this, Continuation]()
					{
						fp_InitApplications();
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
				{
					if (!pApplication->f_IsChildApp())
						pApplication->f_Stop(true) > ApplicationStops.f_AddResult();
				}

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
