// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/EJSON>
#include <Mib/Concurrency/ConcurrencyDefines>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorSingleSubscription>

#include "Malterlib_Cloud_App_KeyManager.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			struct CCommandLineClient
			{
			public:
				CCommandLineClient();
				~CCommandLineClient();
				
				TCDistributedActor<ICCommandLine> f_GetClient(); 
				
			private:
				void fp_Init(); 
				
				TCActor<CDistributedActorTrustManager> mp_TrustManager;
				TCActor<TCDistributedActorSingleSubscription<ICCommandLine>> mp_CommandLineSubscription;
				bool mp_bInitialized = false;
			};
		}		
	}
}
