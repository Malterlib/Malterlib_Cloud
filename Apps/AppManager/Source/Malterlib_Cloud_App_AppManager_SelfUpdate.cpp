// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Daemon/Daemon>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	ch8 const *g_pSelfUpdateScript =
#		include "Malterlib_Cloud_App_AppManager_SelfUpdate.sh"
	;

	TCFuture<bool> CAppManagerActor::fp_SelfUpdate(TCSharedPointer<CApplication> _pApplication)
	{
		if (CFile::fs_GetProgramDirectory() != mp_State.m_RootDirectory)
		{
			co_return DMibErrorInstance
				(
					"Cannot self update when root directory differs from program directory. '{}' != '{}'"_f << CFile::fs_GetProgramDirectory() << mp_State.m_RootDirectory
				)
			;
		}

		auto BlockingActorCheckout = fg_BlockingActor();

		TCAsyncResult<bool> UpdateResult = co_await
			(
				g_Dispatch(BlockingActorCheckout) / [SourceDir = _pApplication->f_GetDirectory(), AutoUpdateDelay = mp_AutoUpdateDelay]() -> bool
				{
					CStr ProgramDir = CFile::fs_GetProgramDirectory();
					CStr ProgramPath = CFile::fs_GetProgramPath();

					CFile::CFindFilesOptions FindFilesOptions(SourceDir / "*", true);
					FindFilesOptions.m_AttribMask = EFileAttrib_File;
					FindFilesOptions.m_ExcludePatterns = {SourceDir / "TempVersion", SourceDir / "TempVersionDownload", SourceDir / ".tmp", SourceDir / ".home"};

					auto Files = CFile::fs_FindFiles(FindFilesOptions);

					bool bUpdatedFiles = false;
					for (auto &File : Files)
					{
						CStr RelativePath = File.m_Path.f_Extract(SourceDir.f_GetLen() + 1);
						CStr Source = CFile::fs_AppendPath(ProgramDir, RelativePath);
						if (CFile::fs_DiffCopyFileOrDirectory(File.m_Path, CFile::fs_AppendPath(ProgramDir, RelativePath), nullptr, {}, 0.0))
							bUpdatedFiles = true;
					}

					if (!bUpdatedFiles)
					{
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "No changes found when running self update");
						return false;
					}

					CStr Script = g_pSelfUpdateScript;

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

					CProcessLaunchParams Params;
					Params.m_bAllowExecutableLocate = true;
					Params.m_WorkingDirectory = ProgramDir;
					Params.m_bMergeEnvironment = true;
					Params.m_Environment["AppManagerPID"] = "{}"_f << NProcess::NPlatform::fg_Process_GetCurrentUID();

					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Running self update script");

					NContainer::TCVector<CStr> CommandLine;
					NSys::fg_Process_GetCommandLineArgs(CommandLine);

					bool bDaemon = false;
					bool bDaemonDebug = false;
					bool bDaemonStandalone = false;
					bool bDaemonSupportRestart = CDaemon::fs_SupportsAutoRestart();

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
						DError("Program not running as daemon or in daemon debug mode, so self update is not supported");

					TCVector<CStr> ExtraLaunchParams;

					if (AutoUpdateDelay != mc_DefaultAutoUpdateDelay && (bDaemonDebug || bDaemonStandalone))
						ExtraLaunchParams.f_Insert("--auto-update-delay={}"_f << AutoUpdateDelay);

					if (!ExtraLaunchParams.f_IsEmpty())
						Params.m_Environment["ExtraLaunchParams"] = CProcessLaunchParams::fs_GetParamsBash(ExtraLaunchParams);

					CStr StdOut;
					CStr StdErr;
					uint32 ExitCode;
					if (bDaemon && bDaemonSupportRestart)
					{
						CDaemon::fs_QuitDaemon(); // Initiate application quit
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Self updating by quitting daemon");
					}
					else if (CProcessLaunch::fs_LaunchBlock(CProcessLaunch::fs_GetBashPath(), fg_CreateVector(FileName, ProgramPath, Type), StdOut, StdErr, ExitCode, Params))
					{
						if (ExitCode)
						{
							DMibError(fg_Format("Self update script failed with exit code {} and reported: {}", ExitCode, fg_ConcatOutput(StdOut, StdErr)));
						}
						else if (bDaemonDebug || bDaemonStandalone)
							CDaemon::fs_QuitDaemon(); // Initiate application quit
					}
					else
						DMibError(fg_Format("Failed to launch self update script: {}", StdErr));

					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Self update restart was initiated successfully {}", fg_ConcatOutput(StdOut, StdErr));

					return true;
				}
			)
			.f_Wrap()
		;

		if (!UpdateResult)
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Self update failed: {}", UpdateResult.f_GetExceptionStr());

		co_return fg_Move(UpdateResult);
	}
}
