// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_InitSensor()
	{
		mp_SensorStore = co_await fp_OpenSensorStoreLocal();

		mp_SensorReporterInterface.f_Construct(mp_State.m_DistributionManager, this);

		co_return {};
	}

	auto CAppManagerActor::CDistributedAppSensorReporterImplementation::f_OpenSensorReporter(CSensorInfo &&_SensorInfo) -> TCFuture<CSensorReporter>
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		CCallingHostInfo CallingHostInfo = NConcurrency::fg_GetCallingHostInfo();

		auto ReportingHostID = CallingHostInfo.f_GetRealHostID();

		auto pApplication = pThis->fp_ApplicationFromHostID(ReportingHostID);
		if (!pApplication)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "[{}] Unassociated application tried to open sensor reporter", CallingHostInfo.f_GetHostInfo().f_GetDesc());
			co_return DErrorInstance("Application not associated with your host");
		}

		auto &Application = *pApplication;

		CDistributedAppSensorReporter::CSensorInfo SensorInfo = fg_Move(_SensorInfo);
		if (SensorInfo.m_HostID)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Error
					, "[{}] Application tried to open sensor reporter with a specified host id: {}"
					, CallingHostInfo.f_GetHostInfo().f_GetDesc()
					, SensorInfo.m_HostID
				)
			;
			co_return DErrorInstance("You cannot specify host id, it's automatically populated with your host id");
		}
		if (SensorInfo.m_Scope.f_IsOfType<CDistributedAppSensorReporter::CSensorScope_Application>())
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Error
					, "[{}] Application tried to open sensor reporter with a specified application scope: {}"
					, CallingHostInfo.f_GetHostInfo().f_GetDesc()
					, SensorInfo.m_Scope.f_GetAsType<CDistributedAppSensorReporter::CSensorScope_Application>().m_ApplicationName
				)
			;
			co_return DErrorInstance("You cannot specify application, it's automatically populated with application");
		}

		SensorInfo.m_HostID = ReportingHostID;
		SensorInfo.m_HostName = CallingHostInfo.f_GetHostInfo().m_FriendlyName;
		SensorInfo.m_Scope = CDistributedAppSensorReporter::CSensorScope_Application{Application.m_Name};

		for (auto &MetaData : pThis->mp_SensorMetaData.f_Entries())
		{
			auto &Key = MetaData.f_Key();

			if (SensorInfo.m_MetaData.f_FindEqual(Key))
				continue;

			SensorInfo.m_MetaData[Key] = MetaData.f_Value();
		}

		auto Reporter = co_await pThis->mp_SensorStore(&CDistributedAppSensorStoreLocal::f_OpenSensorReporter, fg_TempCopy(SensorInfo));

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "[{}] Application '{}' opened sensor reporter: {}"
				, CallingHostInfo.f_GetHostInfo().f_GetDesc()
				, Application.m_Name
				, SensorInfo
			)
		;

		co_return fg_Move(Reporter);
	}
}
