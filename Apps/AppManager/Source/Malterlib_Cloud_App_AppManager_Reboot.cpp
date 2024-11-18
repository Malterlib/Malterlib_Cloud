// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

#include <Mib/Daemon/Daemon>

namespace NMib::NCloud::NAppManager
{
	ch8 const *g_pRebootScript =
#		include "Malterlib_Cloud_App_AppManager_Reboot.sh"
	;

	TCFuture<void> CAppManagerActor::fp_Reboot(bool _bErrorOnPreventReboot)
	{
#ifndef DPlatformFamily_Linux
		co_return {};
#else
		if (mp_bRebooting)
			co_return {};

		if (co_await fp_CheckAndLogPreventedReboot(_bErrorOnPreventReboot))
			co_return {};

		mp_bRebooting = true;

		auto BlockingActorCheckout = fg_BlockingActor();

		auto Result = co_await
			(
				g_Dispatch(BlockingActorCheckout) / []()
				{
					CStr ProgramDir = CFile::fs_GetProgramDirectory();
					CStr ProgramPath = CFile::fs_GetProgramPath();
					
					CProcessLaunchParams Params;
					Params.m_bAllowExecutableLocate = true;
					Params.m_WorkingDirectory = ProgramDir;
					Params.m_bMergeEnvironment = true;
					Params.m_bCreateNewProcessGroup = true;
					Params.m_Environment["AppManagerPID"] = "{}"_f << NProcess::NPlatform::fg_Process_GetCurrentUID();

					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Running reboot script");

					NContainer::TCVector<CStr> CommandLine;
					NSys::fg_Process_GetCommandLineArgs(CommandLine);

					bool bDaemon = false;
					bool bDaemonDebug = false;
					bool bDaemonStandalone = false;

					for (auto &Parameter : CommandLine)
					{
						if (Parameter == "--daemon-run" || Parameter == "-Service")
							bDaemon = true;
						else if (Parameter == "--daemon-run-debug")
							bDaemonDebug = true;
						else if (Parameter == "--daemon-run-standalone")
							bDaemonStandalone = true;
					}

					CStr Type;
					if (bDaemonDebug)
						Type = "DaemonDebug";
					else if (bDaemonStandalone)
						Type = "DaemonStandalone";
					else if (bDaemon)
						Type = "Daemon";
					else
						DError("Program not running as daemon or in daemon debug mode, so reboot is not supported");

					CStr Script = g_pRebootScript;

					CStr FileName = fg_Format("{}/TempScripts/Bash_{}.sh", ProgramDir, CHash_MD5::fs_DigestFromData(Script.f_GetStr(), Script.f_GetLen()).f_GetString());
					if (!CFile::fs_FileExists(FileName))
					{
						CFile::fs_CreateDirectory(CFile::fs_GetPath(FileName));
						CFile::fs_WriteStringToFile
							(
								FileName
								, Script
								, false
								, EFileAttrib_UnixAttributesValid | EFileAttrib_UserExecute | EFileAttrib_UserRead | EFileAttrib_UserWrite
							)
						;
					}

					CStr StdOut;
					CStr StdErr;
					uint32 ExitCode;

					if (CProcessLaunch::fs_LaunchBlock(CProcessLaunch::fs_GetBashPath(), fg_CreateVector(FileName, ProgramPath, Type), StdOut, StdErr, ExitCode, Params))
					{
						if (ExitCode)
						{
							DMibError(fg_Format("Reboot script failed with exit code {} and reported: {}", ExitCode, fg_ConcatOutput(StdOut, StdErr)));
						}
						else if (bDaemonDebug || bDaemonStandalone)
							CDaemon::fs_QuitDaemon(); // Initiate application quit
					}
					else
						DMibError(fg_Format("Failed to launch reboot script: {}", StdErr));

					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Reboot was initiated successfully {}", fg_ConcatOutput(StdOut, StdErr));
				}
			)
			.f_Wrap()
		;

		if (!Result)
		{
			mp_bRebooting = false;
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Reboot failed: {}", Result.f_GetExceptionStr());
			co_return Result.f_GetException();
		}

		co_return {};
#endif
	}
}
