// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/Application>

#include "Malterlib_Cloud_App_AppManager.h"

using namespace NMib;
using namespace NMib::NCloud::NAppManager;

class CAppManager : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudAppManager"
				, "Malterlib Cloud App Manager"
				, "Manages distributed cloud apps running on one host"
				, []
				{
					return fg_ConstructActor<CAppManagerActor>();
				}
			}
		;
		return Daemon.f_Run();
	}	
};

DAppImplement(CAppManager);
