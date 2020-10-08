// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Concurrency/DistributedDaemon>

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
			NCryptography::CHash_SHA256 Hash;

			CStr Salt = "MalterlibAppManagerDaemoName";

			Hash.f_AddData(Salt.f_GetStr(), Salt.f_GetLen());
			Hash.f_AddData(ProgramDirectory.f_GetStr(), ProgramDirectory.f_GetLen());
			DefaultDaemonName = "MalterlibCloudAppManager_{}"_f << Hash.f_GetDigest().f_GetString().f_Left(16);
			Description = "Malterlib Cloud App Manager [{}]"_f << ProgramDirectory;
		}

		NConcurrency::CDistributedDaemon Daemon
			{
				DefaultDaemonName
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
