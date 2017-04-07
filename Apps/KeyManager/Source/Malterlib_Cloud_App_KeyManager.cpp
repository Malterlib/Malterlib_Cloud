// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_KeyManager.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			CKeyManagerDaemonActor::CKeyManagerDaemonActor()
				: CDistributedAppActor(CDistributedAppActor_Settings{"KeyManager", false})
			{
			}
			
			CKeyManagerDaemonActor::~CKeyManagerDaemonActor()
			{
			}

			TCContinuation<void> CKeyManagerDaemonActor::fp_StartApp(NEncoding::CEJSON const &_Params)
			{
				TCContinuation<void> Continuation;
				DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Warning, "Waiting for user to provide password");
				Continuation.f_SetResult();
				return Continuation;				
			}
			
			void CKeyManagerDaemonActor::fp_DatabaseDecrypted()
			{
				DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Password provided, starting up key manager");
				CKeyManagerServerConfig Config;
				Config.m_DatabaseActor = mp_DatabaseActor;
				mp_ServerActor = fg_ConstructActor<CKeyManagerServer>(Config);
			}

			TCContinuation<void> CKeyManagerDaemonActor::fp_StopApp()
			{	
				TCSharedPointer<CCanDestroyTracker> pCanDestroy = fg_Construct();
				
				if (mp_ServerActor || mp_DatabaseActor)
					DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down");
				
				if (mp_ServerActor)
				{
					DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server");
					mp_ServerActor->f_Destroy() > [this, pCanDestroy](TCAsyncResult<void> &&_Result)
						{
							DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Key server shut down");
							if (mp_DatabaseActor)
							{
								DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server database");
								mp_DatabaseActor->f_Destroy() > [this, pCanDestroy](TCAsyncResult<void> &&_Result)
									{
										DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Key server database shut down");
									}
								;
								mp_DatabaseActor = nullptr;
							}
						}
					;
					mp_ServerActor = nullptr;
				}
				else if (mp_DatabaseActor)
				{
					DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server database");
					mp_DatabaseActor->f_Destroy() > pCanDestroy->f_Track();
					mp_DatabaseActor = nullptr;
				}

				return pCanDestroy->m_Continuation;
			}
		}		
	}
}
