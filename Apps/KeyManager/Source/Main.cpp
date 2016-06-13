// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_KeyManager.h"

using namespace NMib;
using namespace NMib::NCloud::NKeyManager;

class CKeyManager : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudKeyManager"
				, "Malterlib Cloud Key Manager"
				, "Manages encrption keys for distributed Malterlib cloud applications"
				, []
				{
					return fg_ConstructActor<CKeyManagerDaemonActor>();
				}
			}
		;
		return Daemon.f_Run();
	}	
};

DAppImplement(CKeyManager);
