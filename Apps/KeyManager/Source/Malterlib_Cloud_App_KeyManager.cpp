// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_KeyManager.h"

namespace NMib::NCloud::NKeyManager
{
	CKeyManagerDaemonActor::CKeyManagerDaemonActor()
		: CDistributedAppActor(CDistributedAppActor_Settings{"KeyManager"})
	{
	}

	CKeyManagerDaemonActor::~CKeyManagerDaemonActor()
	{
	}

	TCFuture<void> CKeyManagerDaemonActor::fp_SetPasswordStatus(CDistributedAppSensorReporter::CStatus _Status)
	{
		auto CheckDestroyOnResume = co_await fp_CheckStoppedOrDestroyedOnResume();

		if (!mp_PasswordStatusReporter)
		{
			CDistributedAppSensorReporter::CSensorInfo SensorInfo;
			SensorInfo.m_Identifier = "org.malterlib.keymanager.password.status";
			SensorInfo.m_Name = "Key Manager Password Status";
			SensorInfo.m_Type = CDistributedAppSensorReporter::ESensorDataType_Status;
			SensorInfo.m_Metadata = mp_SensorMetadata;

			mp_PasswordStatusReporter = co_await fp_OpenSensorReporter(fg_Move(SensorInfo));
		}

		CDistributedAppSensorReporter::CSensorReading Reading;
		Reading.m_Data = fg_Move(_Status);

		co_await mp_PasswordStatusReporter->m_fReportReadings(TCVector<CDistributedAppSensorReporter::CSensorReading>{fg_Move(Reading)}).f_Wrap()
			> fg_LogError("Mib/Cloud/KeyManager/Daemon", "Failed to report readings (Key Manager Password Status)")
		;

		co_return {};
	}

	TCFuture<void> CKeyManagerDaemonActor::fp_StartApp(NEncoding::CEJsonSorted const _Params)
	{
		DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Warning, "Waiting for user to provide password");
		auto CheckDestroyOnResume = co_await fp_CheckStoppedOrDestroyedOnResume();

		co_await fp_SetPasswordStatus({.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Error, .m_Description = "Waiting for user to provide password"});

		co_return {};
	}

	TCFuture<void> CKeyManagerDaemonActor::fp_DatabaseDecrypted()
	{
		DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Password provided, starting up key manager");
		auto CheckDestroyOnResume = co_await fp_CheckStoppedOrDestroyedOnResume();

		co_await fp_SetPasswordStatus({.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Ok, .m_Description = "Password provided"});

		uint32 CreateNewKeyMinServers = fg_Clamp(mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("CreateNewKeyMinServers", 1).f_Integer(), int64(1), int64(100));
		DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Requiring a minimun of {} servers to create a new key", CreateNewKeyMinServers);

		mp_ServerActor = fg_ConstructActor<CKeyManagerServer>
			(
				CKeyManagerServerConfig
				{
					.m_DatabaseActor = mp_DatabaseActor
					, .m_TrustManager = mp_State.m_TrustManager
					, .m_fAuditorFactory = mp_State.f_AuditorFactory()
					, .m_CreateNewKeyMinServers = CreateNewKeyMinServers
				}
			)
		;
		co_await mp_ServerActor(&CKeyManagerServer::f_Init, 10.0);

		co_return {};
	}

	TCFuture<void> CKeyManagerDaemonActor::fp_StopApp()
	{
		CLogError LogError("Mib/Cloud/KeyManager/Daemon");

		if (mp_ServerActor || mp_DatabaseActor)
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down");

		if (mp_ServerActor)
		{
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server");
			co_await fg_Move(mp_ServerActor).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy server actor");
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Key server shut down");
		}

		if (mp_DatabaseActor)
		{
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Shutting down key server database");
			co_await fg_Move(mp_DatabaseActor).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy database actor");
			DMibLogWithCategory(Mib/Cloud/KeyManager/Daemon, Info, "Key server database shut down");
		}

		if (mp_PasswordStatusReporter && mp_PasswordStatusReporter->m_fReportReadings)
			co_await fg_Move(mp_PasswordStatusReporter->m_fReportReadings).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy password status reporter");

		co_return {};
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_KeyManager()
	{
		return fg_Construct<NKeyManager::CKeyManagerDaemonActor>();
	}
}
