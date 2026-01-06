// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JsonShortcuts>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
#ifndef DPlatformFamily_Windows
	static ch8 const *g_pLimitsSetupScript =
#		include "Malterlib_Cloud_App_AppManager_Limits_Setup.sh"
	;
#endif

	TCFuture<void> CAppManagerActor::fp_SetupLimits()
	{
#ifdef DPlatformFamily_Windows
		co_return {};
#else
		if (NProcess::NPlatform::fg_Process_GetElevation() == EProcessElevation_IsNotElevated)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Skipping limits setup because not elevated");
			co_return {};
		}

		uint32 nFiles = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("ResourcesExtraFiles", 256*1024).f_Integer();
		uint32 nMaxFilesPerProc = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("ResourcesMaxFilesPerProcess", 10240).f_Integer();
		uint32 nThreads = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("ResourcesExtraThreads", 65536).f_Integer();
		uint32 nProceses = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("ResourcesExtraProcesses", 32768).f_Integer();
		uint32 nMaxMapCount = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("ResourcesMaxMapCount", 65530).f_Integer();

		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;

			if (Application.m_RegisterInfo.m_Resources_Threads)
				nThreads += *Application.m_RegisterInfo.m_Resources_Threads;

			if (Application.m_RegisterInfo.m_Resources_Files)
				nFiles += *Application.m_RegisterInfo.m_Resources_Files;

			if (Application.m_RegisterInfo.m_Resources_FilesPerProcess)
				nMaxFilesPerProc = fg_Max(nMaxFilesPerProc, *Application.m_RegisterInfo.m_Resources_FilesPerProcess);

			if (Application.m_RegisterInfo.m_Resources_Processes)
				nProceses += *Application.m_RegisterInfo.m_Resources_Processes;

			if (Application.m_RegisterInfo.m_Resources_MaxMapCount)
				nMaxMapCount += *Application.m_RegisterInfo.m_Resources_MaxMapCount;
		}

		TCMap<CStr, CStr> Environment;
		Environment["NumFiles"] = CStr::fs_ToStr(nFiles);
		Environment["NumFilesPerProc"] = CStr::fs_ToStr(nMaxFilesPerProc);
		Environment["NumThreads"] = CStr::fs_ToStr(nThreads);
		Environment["NumPids"] = CStr::fs_ToStr(nProceses);
		Environment["MaxMapCount"] = CStr::fs_ToStr(nMaxMapCount);
		Environment["PlatformFamily"] = DMibStringize(DPlatformFamily);

		co_await
			(
				fp_RunBashScript
				(
					g_pLimitsSetupScript
					, "SetupLimits"
					, fg_Move(Environment)
					, nullptr
					, 2
				)
				% "Failed to setup limits"
			)
		;

		co_return {};
#endif
	}

	void CAppManagerActor::fp_UpdateLimits()
	{
		fp_SetupLimits() > [](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to update limits: {}", _Result.f_GetExceptionStr());
			}
		;
	}
}
