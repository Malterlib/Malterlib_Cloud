// Copyright © 2022 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_InitLog()
	{
		mp_LogStore = co_await fp_OpenLogStoreLocal();

		mp_LogReporterInterface.f_Construct(mp_State.m_DistributionManager, this);

		co_return {};
	}

	auto CAppManagerActor::CDistributedAppLogReporterImplementation::f_OpenLogReporter(CLogInfo &&_LogInfo) -> TCFuture<CLogReporter>
	{
		auto pThis = m_pThis;
		auto OnResume = g_OnResume / [pThis]
			{
				if (pThis->f_IsDestroyed())
					DMibError("Shutting down");
			}
		;

		CCallingHostInfo CallingHostInfo = NConcurrency::fg_GetCallingHostInfo();

		auto ReportingHostID = CallingHostInfo.f_GetRealHostID();

		auto pApplication = pThis->fp_ApplicationFromHostID(ReportingHostID);
		if (!pApplication)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "[{}] Unassociated application tried to open log reporter", CallingHostInfo.f_GetHostInfo().f_GetDesc());
			co_return DErrorInstance("Application not associated with your host");
		}

		auto &Application = *pApplication;

		CDistributedAppLogReporter::CLogInfo LogInfo = fg_Move(_LogInfo);
		if (LogInfo.m_HostID)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Error
					, "[{}] Application tried to open log reporter with a specified host id: {}"
					, CallingHostInfo.f_GetHostInfo().f_GetDesc()
					, LogInfo.m_HostID
				)
			;
			co_return DErrorInstance("You cannot specify host id, it's automatically populated with your host id");
		}
		if (LogInfo.m_Scope.f_IsOfType<CDistributedAppLogReporter::CLogScope_Application>())
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Error
					, "[{}] Application tried to open log reporter with a specified application scope: {}"
					, CallingHostInfo.f_GetHostInfo().f_GetDesc()
					, LogInfo.m_Scope.f_GetAsType<CDistributedAppLogReporter::CLogScope_Application>().m_ApplicationName
				)
			;
			co_return DErrorInstance("You cannot specify application, it's automatically populated with application");
		}

		LogInfo.m_HostID = ReportingHostID;
		LogInfo.m_HostName = CallingHostInfo.f_GetHostInfo().m_FriendlyName;
		LogInfo.m_Scope = CDistributedAppLogReporter::CLogScope_Application{Application.m_Name};

		auto Reporter = co_await pThis->mp_LogStore(&CDistributedAppLogStoreLocal::f_OpenLogReporter, fg_TempCopy(LogInfo));

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "[{}] Application '{}' opened log reporter: {}"
				, CallingHostInfo.f_GetHostInfo().f_GetDesc()
				, Application.m_Name
				, LogInfo
			)
		;

		co_return fg_Move(Reporter);
	}
}
