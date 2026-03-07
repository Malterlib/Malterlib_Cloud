// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_SecretsManager.h"

using namespace NMib;
using namespace NMib::NCloud::NSecretsManager;

struct CSecretsManagerApp : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudSecretsManager"
				, "Malterlib Cloud Secrets Manager"
				, "Manages secrets"
				, []
				{
					return fg_ConstructActor<CSecretsManagerDaemonActor>();
				}
			}
		;

		return Daemon.f_Run();
	}

};

DAppImplement(CSecretsManagerApp);
