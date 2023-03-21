// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_TestApp.h"

using namespace NMib;

namespace NMib::NCloud::NTest
{
	CTestAppActor::CTestAppActor()
		: CDistributedAppActor(CDistributedAppActor_Settings("TestApp"))
	{
	}

	void CTestAppActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);
		o_CommandLine.f_SetProgramDescription
			(
				"Test App"
				, "Test App."
			)
		;
		o_CommandLine.f_RegisterGlobalOptions
			(
				{
					"UpdateType?"_=
					{
						"Names"_= {"--update-type"}
						,"Type"_= COneOf{"Independent", "OneAtATime", "AllAtOnce"}
						, "Description"_= "Override the update type for the application."
					}
				}
			)
		;

		o_CommandLine.f_GetDefaultSection().f_RegisterCommand
			(
				{
					"Names"_= {"--generate-sensor-readings"}
					, "Description"_= "Generates a sensor reading."
					, "Parameters"_=
					{
						"NumReadings?"_=
						{
							"Default"_= 1
							, "Description"_= "The number of readings to report."
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					CDistributedAppSensorReporter::CSensorInfo SensorInfo;
					SensorInfo.m_Identifier = "org.malterlib.testapp.test";
					SensorInfo.m_Name = "Test Sensor";
					SensorInfo.m_Type = NConcurrency::CDistributedAppSensorReporter::ESensorDataType_Float;
					SensorInfo.m_UnitDivisors = CDistributedAppSensorReporter::fs_DiskSpaceDivisors();
					auto SensorReporter = co_await self(&CTestAppActor::fp_OpenSensorReporter, fg_Move(SensorInfo));

					if (!SensorReporter.m_fReportReadings)
						co_return DMibErrorInstance("Invalid sensor reporter returned");

					mint nReadings = _Params["NumReadings"].f_Integer();

					TCVector<CDistributedAppSensorReporter::CSensorReading> Readings;
					for (mint i = 0; i < nReadings; ++i)
					{
						CDistributedAppSensorReporter::CSensorReading SensorReading;
						SensorReading.m_Data = fg_GetRandomFloat().f_Pow(10)*1024.0*1024.0*1024.0*1024.0*1024.0;
						Readings.f_Insert(fg_Move(SensorReading));
					}

					co_await SensorReporter.m_fReportReadings(fg_Move(Readings));

					co_await fg_Move(SensorReporter.m_fReportReadings).f_Destroy();

					co_return 0;
				}
			)
		;
		o_CommandLine.f_GetDefaultSection().f_RegisterCommand
			(
				{
					"Names"_= {"--set-sensor-status"}
					, "Description"_= "Set sensor status."
					, "Parameters"_=
					{
						"Status"_=
						{
							"Type"_= COneOf{"Ok", "Info", "Warning", "Error"}
							, "Default"_= "Info"
							, "Description"_= "The number of readings to report."
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					CDistributedAppSensorReporter::CSensorInfo SensorInfo;
					SensorInfo.m_Identifier = "org.malterlib.testapp.test.status";
					SensorInfo.m_Name = "Test Sensor (status)";
					SensorInfo.m_Type = NConcurrency::CDistributedAppSensorReporter::ESensorDataType_Status;

					auto SensorReporter = co_await self(&CTestAppActor::fp_OpenSensorReporter, fg_Move(SensorInfo));

					if (!SensorReporter.m_fReportReadings)
						co_return DMibErrorInstance("Invalid sensor reporter returned");

					CDistributedAppSensorReporter::CStatus Status;

					auto &StatusStr = _Params["Status"].f_String();

					Status.m_Description = "Test status '{}'"_f << StatusStr;

					if (StatusStr == "Ok")
						Status.m_Severity = NConcurrency::CDistributedAppSensorReporter::EStatusSeverity_Ok;
					else if (StatusStr == "Info")
						Status.m_Severity = NConcurrency::CDistributedAppSensorReporter::EStatusSeverity_Info;
					else if (StatusStr == "Warning")
						Status.m_Severity = NConcurrency::CDistributedAppSensorReporter::EStatusSeverity_Warning;
					else if (StatusStr == "Error")
						Status.m_Severity = NConcurrency::CDistributedAppSensorReporter::EStatusSeverity_Error;

					{
						TCVector<CDistributedAppSensorReporter::CSensorReading> Readings;
						auto &SensorReading = Readings.f_Insert();
						SensorReading.m_Data = fg_Move(Status);

						co_await SensorReporter.m_fReportReadings(fg_Move(Readings));
					}

					co_await fg_Move(SensorReporter.m_fReportReadings).f_Destroy();

					co_return 0;
				}
			)
		;
		o_CommandLine.f_GetDefaultSection().f_RegisterCommand
			(
				{
					"Names"_= {"--generate-log-entries"}
					, "Description"_= "Generates a log entries."
					, "Parameters"_=
					{
						"NumEntries?"_=
						{
							"Default"_= 1
							, "Description"_= "The number of entries to report."
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					mint nEntries = _Params["NumEntries"].f_Integer();

					CDistributedAppLogReporter::CLogInfo LogInfo;

					LogInfo.m_Identifier = "org.malterlib.log.test";
					LogInfo.m_IdentifierScope = "Test";
					LogInfo.m_Name = "Malterlib Test";

					auto LogReporter = co_await self(&CTestAppActor::fp_OpenLogReporter, LogInfo);

					TCVector<CDistributedAppLogReporter::CLogEntry> LogEntries;

					for (mint i = 0; i < nEntries; ++i)
					{
						CDistributedAppLogReporter::CLogEntry LogEntry;
						LogEntry.m_Data.m_Message = "Test Log {}"_f << i;
						LogEntries.f_Insert(fg_Move(LogEntry));
					}

					co_await LogReporter.m_fReportEntries(fg_Move(LogEntries));

					co_await fg_Move(LogReporter.m_fReportEntries).f_Destroy();

					co_return 0;
				}
			)
		;
		o_CommandLine.f_GetDefaultSection().f_RegisterCommand
			(
				{
					"Names"_= {"--get-log-report-depth"}
					, "Description"_= "Gets the depth of the log reporting chain."
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					CDistributedAppLogReporter::CLogInfo LogInfo;

					LogInfo.m_Identifier = "org.malterlib.log.test";
					LogInfo.m_IdentifierScope = "Test";
					LogInfo.m_Name = "Malterlib Test";

					auto LogReporter = co_await self(&CTestAppActor::fp_OpenLogReporter, LogInfo);

					TCVector<CDistributedAppLogReporter::CLogEntry> LogEntries;

					auto ReportResult = co_await LogReporter.m_fReportEntries(fg_Move(LogEntries));

					co_await fg_Move(LogReporter.m_fReportEntries).f_Destroy();

					co_await _pCommandLine->f_StdOut(CStrSecure::CFormat("{}\n") << ReportResult.m_ReportDepth);

					co_return 0;
				}
			)
		;
		o_CommandLine.f_GetDefaultSection().f_RegisterCommand
			(
				{
					"Names"_= {"--generate-huge-log-entries"}
					, "Description"_= "Generates a log entries."
					, "Options"_=
					{
						"NumEntries?"_=
						{
							"Names"_= {"--num-entries"}
							, "Default"_= 1
							, "Description"_= "The number of entries to report."
						}
						, "EntrySize?"_=
						{
							"Names"_= {"--entry-size"}
							, "Default"_= 1024
							, "Description"_= "The size of the entry in bytes."
						}
						, "LineSize?"_=
						{
							"Names"_= {"--line-size"}
							, "Default"_= 128
							, "Description"_= "The size of the lines in bytes."
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					mint nEntries = _Params["NumEntries"].f_Integer();
					mint EntrySize = _Params["EntrySize"].f_Integer();
					mint LineSize = _Params["LineSize"].f_Integer();

					if (LineSize < 1 || LineSize > EntrySize)
						co_return DMibErrorInstance("Invalid line size");

					CDistributedAppLogReporter::CLogInfo LogInfo;

					LogInfo.m_Identifier = "org.malterlib.log.test";
					LogInfo.m_IdentifierScope = "Test";
					LogInfo.m_Name = "Malterlib Test";

					auto LogReporter = co_await self(&CTestAppActor::fp_OpenLogReporter, LogInfo);

					TCVector<CDistributedAppLogReporter::CLogEntry> LogEntries;

					for (mint i = 0; i < nEntries; ++i)
					{
						CDistributedAppLogReporter::CLogEntry LogEntry;
						for (mint iData = 0; iData < EntrySize;)
						{
							mint ThisTime = fg_Min(EntrySize - iData, LineSize);
							if (ThisTime >= 3)
								LogEntry.m_Data.m_Message += "<{sj*,sf }>\n"_f << "" << (ThisTime - 3);
							else
								LogEntry.m_Data.m_Message += "{sj*,sf#}\n"_f << "" << (ThisTime - 1);

							iData += ThisTime;
						}
						LogEntries.f_Insert(fg_Move(LogEntry));
					}

					co_await LogReporter.m_fReportEntries(fg_Move(LogEntries));

					co_await fg_Move(LogReporter.m_fReportEntries).f_Destroy();

					co_return 0;
				}
			)
		;
	}

	void CTestAppActor::fp_PopulateAppInterfaceRegisterInfo(CDistributedAppInterfaceServer::CRegisterInfo &o_RegisterInfo, NEncoding::CEJSON const &_Params)
	{
		if (auto pValue = _Params.f_GetMember("UpdateType", EJSONType_String))
		{
			CStr UpdateType = pValue->f_String();
			if (UpdateType == "Independent")
				o_RegisterInfo.m_UpdateType = EDistributedAppUpdateType_Independent;
			else if (UpdateType == "OneAtATime")
				o_RegisterInfo.m_UpdateType = EDistributedAppUpdateType_OneAtATime;
			else if (UpdateType == "AllAtOnce")
				o_RegisterInfo.m_UpdateType = EDistributedAppUpdateType_AllAtOnce;
		}
	}

	TCFuture<void> CTestAppActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		co_return {};
	}

	TCFuture<void> CTestAppActor::fp_StopApp()
	{
		co_return {};
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_TestApp()
	{
		return fg_Construct<NTest::CTestAppActor>();
	}
}
