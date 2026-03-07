// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/Application>

#ifdef DPlatformFamily_Windows
#include <Windows.h>
#endif

#include "Malterlib_Cloud_App_AppDistributionManager.h"

using namespace NMib;
using namespace NMib::NCloud::NAppDistributionManager;

struct CAppDistributionManager : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudAppDistributionManager"
				, "Malterlib Cloud App Distribution Manager"
				, "Manages distributed cloud apps running on one host"
				, []
				{
					return fg_ConstructActor<CAppDistributionManagerActor>();
				}
			}
		;

		return Daemon.f_Run();
	}
};

DAppImplement(CAppDistributionManager);
