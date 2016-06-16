// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NAppManager
		{
			namespace
			{
				CStr fg_ConcatOutput(CStr const &_StdOut, CStr const &_StdErr)
				{
					if (_StdOut.f_IsEmpty() && _StdErr.f_IsEmpty())
						return CStr();
					CStr Ret;
					CStr StdOut = _StdOut.f_Trim();
					if (!StdOut.f_IsEmpty())
						fg_AddStrSep(Ret, StdOut, DMibNewLine);
					CStr StdErr = _StdErr.f_Trim();
					if (!StdErr.f_IsEmpty())
						fg_AddStrSep(Ret, StdErr, DMibNewLine);
					return DMibNewLine + Ret;
				}
			}

			CStr CAppManagerActor::fsp_RunTool
				(
					CStr const &_Description
					, CStr const &_Tool
					, CStr const &_WorkingDir
					, TCVector<CStr> const &_Arguments
					, CStr const &_Home
					, CStr const &_User
					, TCMap<CStr, CStr> const &_Environment
					, bool _bQuiet
				)
			{
				CProcessLaunchParams Params;
				Params.m_bAllowExecutableLocate = true;
				Params.m_WorkingDirectory = _WorkingDir;
				if (!_User.f_IsEmpty())
				{
					Params.m_RunAsUser = _User;
					Params.m_RunAsGroup = _User;
				}
				if (!_Home.f_IsEmpty())
				{
					Params.m_Environment["HOME"] = _Home;
					Params.m_Environment["TMPDIR"] = _Home + "/.tmp";
				}
				
				Params.m_Environment += _Environment;
				Params.m_bMergeEnvironment = true;
				
				if (!_bQuiet)
					DMibLog(Info, "{cc}", _Description);
				
				CStr StdOut;
				CStr StdErr;
				uint32 ExitCode;
				if (CProcessLaunch::fs_LaunchBlock(_Tool, _Arguments, StdOut, StdErr, ExitCode, Params))
				{
					if (ExitCode)
						DMibError(fg_Format("{cc} failed with exit code {} and reported: {}", _Description, ExitCode, fg_ConcatOutput(StdOut, StdErr)));
				}
				else
					DMibError(fg_Format("Failed to launch {} when {}: {}", _Tool, _Description, StdErr));
				if (!_bQuiet)
					DMibLog(Info, "{} was successful {}", _Description, fg_ConcatOutput(StdOut, StdErr));
				
				return StdOut;
			}
		}
	}
}
