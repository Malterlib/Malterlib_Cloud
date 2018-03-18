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

		CStr ProgramDirectory = NFile::CFile::fs_GetProgramDirectory();
#ifdef DPlatformFamily_Windows
		CStr DefaultProgramDirectory = "c:/M";
#else
		CStr DefaultProgramDirectory = "/M";
#endif

		CStr DefaultDaemonName = "MalterlibCloudAppManager";
		CStr Description = "Malterlib Cloud App Manager";

		if (ProgramDirectory != DefaultProgramDirectory)
		{
			NDataProcessing::CHash_SHA256 Hash;

			CStr Salt = "MalterlibAppManagerDaemoName";

			Hash.f_AddData(Salt.f_GetStr(), Salt.f_GetLen());
			Hash.f_AddData(ProgramDirectory.f_GetStr(), ProgramDirectory.f_GetLen());
			DefaultDaemonName = "MalterlibCloudAppManager_{}"_f << Hash.f_GetDigest().f_GetString().f_Left(16);
			Description = "Malterlib Cloud App Manager [{}]"_f << ProgramDirectory;
		}

		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudAppManager"
				, Description
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
