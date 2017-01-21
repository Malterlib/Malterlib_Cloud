// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Daemon/Daemon>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	ch8 const *g_pSelfUpdateScript =
#		include "Malterlib_Cloud_App_AppManager_SelfUpdate.sh"
	;
	void CAppManagerActor::fp_SelfUpdate(TCSharedPointer<CApplication> const &_pApplication)
	{
		fg_Dispatch
			(
				mp_FileActor
				, [SourceDir = _pApplication->f_GetDirectory()]
				{
					CStr ProgramDir = CFile::fs_GetProgramDirectory();
					CStr ProgramPath = CFile::fs_GetProgramPath();
					auto Files = CFile::fs_FindFiles(SourceDir + "/*", EFileAttrib_File, true);
					bool bUpdatedFiles = false;
					for (auto &File : Files)
					{
						CStr RelativePath = File.f_Extract(SourceDir.f_GetLen() + 1);
						CStr Source = CFile::fs_AppendPath(ProgramDir, RelativePath);
						if (CFile::fs_DiffCopyFileOrDirectory(File, CFile::fs_AppendPath(ProgramDir, RelativePath), nullptr, 0.0))
							bUpdatedFiles = true;
					}
					
					if (!bUpdatedFiles)
						return;
					
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
					
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Running self update script");
					
					NContainer::TCVector<CStr> CommandLine;
					NSys::fg_Process_GetCommandLineArgs(CommandLine);
					
					bool bDaemon = false;
					bool bDaemonDebug = false;
					bool bDaemonSupportRestart = CService::fs_SupportsAutoRestart();
					
					for (auto &Parameter : CommandLine)
					{
						if (Parameter == "--daemon-run" || Parameter == "-Service")
							bDaemon = true;
						else if (Parameter == "--daemon-run-debug")
							bDaemonDebug = true;
					}
					
					CStr Type;
					if (bDaemonDebug)
						Type = "DaemonDebug";
					else if (bDaemon)
						Type = "Daemon";
					else 
						DError("Program not running as daemon or in daemon debug mode, so self update is not supported");
					
					CStr StdOut;
					CStr StdErr;
					uint32 ExitCode;
					if (bDaemon && bDaemonSupportRestart)
					{
						CService::fs_QuitDaemon(); // Initiate application quit
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Self updating by quitting daemon");
					}
					else if (CProcessLaunch::fs_LaunchBlock("bash", fg_CreateVector(FileName, ProgramPath, Type), StdOut, StdErr, ExitCode, Params))
					{
						if (ExitCode)
						{
							DMibError(fg_Format("Self update script failed with exit code {} and reported: {}", ExitCode, fg_ConcatOutput(StdOut, StdErr)));
						}
						else if (bDaemonDebug)
							CService::fs_QuitDaemon(); // Initiate application quit
					}
					else
						DMibError(fg_Format("Failed to launch self update script: {}", StdErr));

					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Self update restart was initiated successfully {}", fg_ConcatOutput(StdOut, StdErr));
				}
			)
			> [this](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Self update failed: {}", _Result.f_GetExceptionStr());
					return;
				}
			}
		;
	}
}
