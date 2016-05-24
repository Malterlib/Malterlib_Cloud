// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_KeyManager_CommandLineClient.h"
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>
#include <Mib/Concurrency/ActorCallOnce>

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			CCommandLineClient::CCommandLineClient()
				: mp_TrustManager
				(
					fg_ConstructActor<CDistributedActorTrustManager>
					(
						fg_ConstructActor<CDistributedActorTrustManagerDatabase_JSONDirectory>(CFile::fs_GetProgramDirectory() + "/CommandLineTrustDatabase")
					)
				) 
			{
			}
			
			CCommandLineClient::~CCommandLineClient()
			{
				fp_Init();
			}
			
			void CCommandLineClient::fp_Init()
			{
				if (!mp_bInitialized)
				{
					mp_TrustManager(&CDistributedActorTrustManager::f_Initialize).f_CallSync(60.0);
					mp_CommandLineSubscription = fg_ConstructActor<TCDistributedActorSingleSubscription<ICCommandLine>>("Malterlib/Concurrency/Commandline");
					mp_bInitialized = true;
				}
			}
				
			TCDistributedActor<ICCommandLine> CCommandLineClient::f_GetClient()
			{
				fp_Init();
				return mp_CommandLineSubscription(&TCDistributedActorSingleSubscription<ICCommandLine>::f_GetActor).f_CallSync(10.0);
			}
		}		
	}
}
