// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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

	auto CAppManagerActor::CDistributedAppSensorReporterImplementation::f_OpenSensorReporter(CSensorInfo _SensorInfo) -> TCFuture<CSensorReporter>
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		CCallingHostInfo CallingHostInfo = NConcurrency::fg_GetCallingHostInfo();

		auto ReportingHostID = CallingHostInfo.f_GetRealHostID();

		auto pApplication = pThis->fp_ApplicationFromHostID(ReportingHostID);

		TCSharedPointer<CEnvironment> pEnvironment;
		if (!pApplication)
			pEnvironment = pThis->fp_EnvironmentFromHostID(ReportingHostID);

		CStr ScopeName;
		if (pApplication)
			ScopeName = pApplication->m_Name;
		else if (pEnvironment)
			ScopeName = fg_Format("Environment/{}", pEnvironment->m_Name);

		if (ScopeName.f_IsEmpty())
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "[{}] Unassociated application tried to open sensor reporter", CallingHostInfo.f_GetHostInfo().f_GetDesc());
			co_return DErrorInstance("Application not associated with your host");
		}

		CDistributedAppSensorReporter::CSensorInfo SensorInfo = fg_Move(_SensorInfo);
		if (pEnvironment && SensorInfo.m_HostID)
		{
			// The environment agent forwards its local sensor store on behalf of the
			// applications running inside the environment, so keep the reporting
			// host and scope the application under the environment
			if (SensorInfo.m_Scope.f_IsOfType<CDistributedAppSensorReporter::CSensorScope_Application>())
			{
				auto &ApplicationName = SensorInfo.m_Scope.f_GetAsType<CDistributedAppSensorReporter::CSensorScope_Application>().m_ApplicationName;
				if (!ApplicationName.f_IsEmpty())
					ScopeName = fg_Format("{}/{}", ScopeName, ApplicationName);
			}

			SensorInfo.m_Scope = CDistributedAppSensorReporter::CSensorScope_Application{ScopeName};
		}
		else
		{
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
			SensorInfo.m_Scope = CDistributedAppSensorReporter::CSensorScope_Application{ScopeName};
		}

		for (auto &Metadata : pThis->mp_SensorMetadata.f_Entries())
		{
			auto &Key = Metadata.f_Key();

			if (SensorInfo.m_Metadata.f_FindEqual(Key))
				continue;

			SensorInfo.m_Metadata[Key] = Metadata.f_Value();
		}

		auto Reporter = co_await pThis->mp_SensorStore(&CDistributedAppSensorStoreLocal::f_OpenSensorReporter, fg_TempCopy(SensorInfo));

		if (pApplication)
			co_await pThis->fp_RebootPrevention_WatchSensor(pApplication->m_Name, SensorInfo);

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "[{}] Application '{}' opened sensor reporter:\n{}"
				, CallingHostInfo.f_GetHostInfo().f_GetDesc()
				, ScopeName
				, SensorInfo
			)
		;

		co_return fg_Move(Reporter);
	}
}
