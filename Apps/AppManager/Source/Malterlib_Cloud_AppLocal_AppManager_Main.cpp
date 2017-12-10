// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/Application>

#ifdef DPlatformFamily_Windows
#include <Windows.h>
#endif

#include "Malterlib_Cloud_App_AppManager.h"

using namespace NMib;
using namespace NMib::NCloud::NAppManager;

class CAppManager : public CApplication
{
	aint f_Main()
	{
#ifdef DPlatformFamily_Windows
		AllocConsole();
		fg_GetSys()->f_SetEnvironmentVariable("Path", "c:\\Program Files\\Git\\usr\\bin;{}"_f << fg_GetSys()->f_GetEnvironmentVariable("Path"));
		NSys::fg_Process_SetEnvironmentVariable_Unsafe("Path", "c:\\Program Files\\Git\\usr\\bin;{}"_f << fg_GetSys()->f_GetEnvironmentVariable("Path"));
#endif

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
