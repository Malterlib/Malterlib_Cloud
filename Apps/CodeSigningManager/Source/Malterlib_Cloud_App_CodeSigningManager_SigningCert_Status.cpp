// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_CodeSigningManager.h"

#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NCodeSigningManager
{
	TCFuture<void> CCodeSigningManagerActor::fp_SigningCert_UpdateSensor(CSigningCertKey _SigningCertKey)
	{
		CSigningCert *pSigningCert = nullptr;

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> NException::CExceptionPointer
				{
					if (mp_State.m_bStoppingApp || f_IsDestroyed())
						return DMibErrorInstance("Startup aborted");

					pSigningCert = mp_SigningCerts.f_FindEqual(_SigningCertKey);

					if (!pSigningCert)
						return DMibErrorInstance("Certificate signing certificate '{}' deleted"_f << _SigningCertKey);

					return {};
				}
			)
		;

		if (!pSigningCert->m_bSensorsRegistered)
			co_await fp_SigningCert_RegisterSensors(_SigningCertKey);

		if (!pSigningCert->m_SensorReporter_Status.m_fReportReadings)
			co_return {};

		CTime Now = CTime::fs_NowUTC();
		CTime MinExpireTime = Now + CTimeSpanConvert::fs_CreateDaySpan(365);

		CDistributedAppSensorReporter::CStatus Status;
		try
		{
			auto ExpireTime = CCertificate::fs_GetCertificateExpirationTime(pSigningCert->m_Certificate);
			if (ExpireTime < Now)
			{
				Status.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Error;
				Status.m_Description = "Error: Certificate expired {} ago"_f << fg_SecondsDurationToHumanReadable(CTimeSpanConvert(Now - ExpireTime).f_GetSecondsFloat());
			}
			else if (ExpireTime < MinExpireTime)
			{
				Status.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Warning;
				Status.m_Description = "Warning: certificate will expire in {}"_f << fg_SecondsDurationToHumanReadable(CTimeSpanConvert(ExpireTime - Now).f_GetSecondsFloat());
			}
			else
			{
				Status.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Ok;
				Status.m_Description = "Certificate will expire in {}"_f << fg_SecondsDurationToHumanReadable(CTimeSpanConvert(ExpireTime - Now).f_GetSecondsFloat());
			}
		}
		catch (CException const &_Exception)
		{
			Status.m_Severity = CDistributedAppSensorReporter::EStatusSeverity_Error;
			Status.m_Description = "Error checkng certificate expiry: {}"_f << _Exception;
		}

		TCVector<CDistributedAppSensorReporter::CSensorReading> SensorReadings;
		SensorReadings.f_Insert().m_Data = Status;

		co_await pSigningCert->m_SensorReporter_Expire.m_fReportReadings(fg_Move(SensorReadings));

		co_return {};
	}

	TCFuture<void> CCodeSigningManagerActor::fp_SigningCert_UpdateSensors()
	{
		TCVector<CSigningCertKey> SigningCerts;
		for (auto &SigningCert : mp_SigningCerts)
			SigningCerts.f_Insert(SigningCert.f_GetKey());

		for (auto &SigningCertKey : SigningCerts)
			fp_SigningCert_UpdateSensor(SigningCertKey) > fg_LogError("Update sensors", "Falied to update signing certificate sensors");

		co_return {};
	}

	TCFuture<void> CCodeSigningManagerActor::fp_SigningCert_RegisterSensors(CSigningCertKey _SigningCertKey)
	{
		CSigningCert *pSigningCert = nullptr;
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> NException::CExceptionPointer
				{
					pSigningCert = mp_SigningCerts.f_FindEqual(_SigningCertKey);

					if (!pSigningCert)
						return DMibErrorInstance("Signing certificate '{}' deleted"_f << _SigningCertKey);

					return {};
				}
			)
		;

		if (pSigningCert->m_bSensorsRegistered)
			co_return {};

		co_await mp_InitSensorReporterSequencer.f_Sequence();

		if (pSigningCert->m_bSensorsRegistered)
			co_return {};

		{
			CDistributedAppSensorReporter::CSensorInfo SensorInfo;
			SensorInfo.m_Identifier = "org.malterlib.codesign.signing-manager.signingcert.certificate-expire";
			SensorInfo.m_IdentifierScope = "{}"_f << _SigningCertKey;
			SensorInfo.m_Name = "CodeSign Signing Certificate Expire";
			SensorInfo.m_ExpectedReportInterval = 24.0 * 60.0 * 60.0;
			SensorInfo.m_Type = NConcurrency::CDistributedAppSensorReporter::ESensorDataType_Status;
			pSigningCert->m_SensorReporter_Expire = co_await fp_OpenSensorReporter(fg_Move(SensorInfo));
		}
		{
			CDistributedAppSensorReporter::CSensorInfo SensorInfo;
			SensorInfo.m_Identifier = "org.malterlib.codesign.signing-manager.signingcert.status";
			SensorInfo.m_IdentifierScope = "{}"_f << _SigningCertKey;
			SensorInfo.m_Name = "CodeSign Signing Certificate Status";
			SensorInfo.m_Type = NConcurrency::CDistributedAppSensorReporter::ESensorDataType_Status;
			pSigningCert->m_SensorReporter_Status = co_await fp_OpenSensorReporter(fg_Move(SensorInfo));
		}

		pSigningCert->m_bSensorsRegistered = true;

		co_return {};
	}

	TCFuture<void> CCodeSigningManagerActor::fp_SigningCert_UpdateStatusSensor(CSigningCertKey _SigningCertKey, EStatusSeverity _Severity, CStr _Status)
	{
		CSigningCert *pSigningCert = nullptr;
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> NException::CExceptionPointer
				{
					pSigningCert = mp_SigningCerts.f_FindEqual(_SigningCertKey);

					if (!pSigningCert)
						return DMibErrorInstance("Signing certificate '{}' deleted"_f << _SigningCertKey);

					return {};
				}
			)
		;

		if (!pSigningCert->m_bSensorsRegistered)
			co_await fp_SigningCert_RegisterSensors(_SigningCertKey);

		if (!pSigningCert->m_SensorReporter_Status.m_fReportReadings)
			co_return {};

		CDistributedAppSensorReporter::CStatus Status;
		Status.m_Severity = _Severity;
		Status.m_Description = _Status;

		TCVector<CDistributedAppSensorReporter::CSensorReading> SensorReadings;
		SensorReadings.f_Insert().m_Data = Status;

		co_await pSigningCert->m_SensorReporter_Status.m_fReportReadings(fg_Move(SensorReadings));

		co_return {};
	}

	void CCodeSigningManagerActor::fp_SigningCert_UpdateStatus(CSigningCert &o_SigningCert, EStatusSeverity _Severity, CStr const &_Status)
	{
		auto Severity = _Severity;
		auto Status = _Status;
		CTime Modified;
		if (_Severity == CDistributedAppSensorReporter::EStatusSeverity_Ok)
		{
			for (auto &LastModified : o_SigningCert.m_SecretsManagers)
			{
				if (!Modified.f_IsValid())
					Modified = LastModified;

				if (LastModified != Modified)
				{
					Severity = CDistributedAppSensorReporter::EStatusSeverity_Warning;
					Status = "Not all managers are up to date";
					break;
				}
			}
		}

		if (Status != o_SigningCert.m_Status.m_Description || Severity != o_SigningCert.m_Status.m_Severity)
		{
			o_SigningCert.m_Status.m_Description = Status;
			o_SigningCert.m_Status.m_Severity = Severity;
			fp_SigningCert_UpdateStatusSensor(o_SigningCert.f_GetKey(), Severity, Status) > fg_LogError("Update Status", "Update signing certificate status sensor failed");

			DLogWithCategory(Mib/Cloud/CodeSigningManager, Info, "<{}> Changing signing certificate status: {}", o_SigningCert.f_GetKey(), _Status);
		}
	}
}
