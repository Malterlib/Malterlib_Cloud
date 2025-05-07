// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Test_Malterlib_Cloud_AppManager.h"

#ifdef DPlatformFamily_Windows
#include <Windows.h>
#endif

namespace
{
	fp64 g_Timeout = 120.0 * NMib::NTest::gc_TimeoutMultiplier;
	constexpr EJsonDialectFlag gc_JsonFlags = EJsonDialectFlag_AllowUndefined | EJsonDialectFlag_AllowInvalidFloat;

	CEJsonSorted fg_FromString(CStr &&_String)
	{
		return CEJsonSorted::fs_FromString
			(
				fg_Move(_String)
				, CStr()
				, false
				, gc_JsonFlags
			)
		;
	}
}

class CAppManager_Sensor_Tests : public NMib::NTest::CTest
{
public:
	void f_DoTests()
	{
		DMibTestSuite("General") -> TCFuture<void>
		{
			CAppManagerTestHelper::EOption Options
				= CAppManagerTestHelper::EOption_LaunchTestAppInApp
				| CAppManagerTestHelper::EOption_EnableVersionManager
				| CAppManagerTestHelper::EOption_DisablePatchMonitoring
				| CAppManagerTestHelper::EOption_DisableDiskMonitoring
				| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
				| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
			;

			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("AppManagerSensorTests", Options, g_Timeout);

			auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

			co_await AppManagerTestHelper.f_Setup(1);
			auto &AppManagerInfo = *AppManagerTestHelper.m_pState->m_AppManagerInfos.f_FindAny();
			auto fShouldKeepValue = [](CEJsonSorted const &_Reading)
				{
					return _Reading["Identifier"].f_String().f_StartsWith("org.malterlib.testapp.test");
				}
			;

			auto fMakeComparable = [&](CEJsonSorted &&_Readings)
				{
					for (auto &Reading : _Readings.f_Array())
					{
						Reading.f_RemoveMember("Timestamp");
						if (!fShouldKeepValue(Reading))
							 Reading.f_RemoveMember("Value");
					}

					// We need to sort by sequence because the system clock might change
					_Readings.f_Array().f_Sort
						(
							[](CEJsonSorted const &_Left, CEJsonSorted const &_Right)
							{
								auto Left = _Left.f_GetMemberValue("UniqueSequence", 0);
								auto Right = _Right.f_GetMemberValue("UniqueSequence", 0);

								return Left <=> Right;
							}
						)
					;

					return fg_Move(_Readings);
				}
			;

			auto TestAppDirectory = AppManagerInfo.m_RootDirectory / "App/TestApp";

			CEJsonSorted ExpectedSensors = _
				[
					_=
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.testapp.test.version"
						, "IdentifierScope"_= ""
						, "Name"_= "Test Sensor (version)"
						, "Type"_= 5
						, "ExpectedReportInterval"_= fp64::fs_Inf()
						, "Flags"_= _[]
						, "PauseReportingFor"_= fp64::fs_QNan()
						, "UnitDivisors"_= _[]
						, "WarnValue"_= nullptr
						, "CriticalValue"_= nullptr
						, "Removed"_= false
						, "SnoozeUntil"_= NTime::CTime()
						, "SensorMetaData"_= _={}
					}
					, _=
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.testapp.test"
						, "IdentifierScope"_= ""
						, "Name"_= "Test Sensor"
						, "Type"_= 2
						, "ExpectedReportInterval"_= fp64::fs_Inf()
						, "Flags"_= _[]
						, "PauseReportingFor"_= fp64::fs_QNan()
						, "UnitDivisors"_= _
						[
							_=
							{
								"Divisor"_= 0.0
								, "nDecimals"_= 1
								, "UnitFormatter"_= "{fn0} B"
							}
							,
							_=
							{
								"Divisor"_= 1024.0
								, "nDecimals"_= 1
								, "UnitFormatter"_= "{fn1} KiB"
							}
							,
							_=
							{
								"Divisor"_= 1048576.0
								, "nDecimals"_= 1
								, "UnitFormatter"_= "{fn1} MiB"
							}
							,
							_=
							{
								"Divisor"_= 1073741824.0
								, "nDecimals"_= 1
								, "UnitFormatter"_= "{fn1} GiB"
							}
							,
							_=
							{
								"Divisor"_= 1099511627776.0
								, "nDecimals"_= 1
								, "UnitFormatter"_= "{fn1} TiB"
							}
						]
						, "WarnValue"_= nullptr
						, "CriticalValue"_= nullptr
						, "Removed"_= false
						, "SnoozeUntil"_= NTime::CTime()
						, "SensorMetaData"_= _={}
					}
				]
			;

			CEJsonSorted ExpectedSensorReadings;
			for (mint i = 0; i < 5; ++i)
			{
				ExpectedSensorReadings.f_Array().f_Insert
					(
						_=
						{
							"HostID"_= ""
							, "HostName"_= ""
							, "Application"_= ""
							, "Identifier"_= "org.malterlib.testapp.test"
							, "IdentifierScope"_= ""
							, "Name"_= "Test Sensor"
							, "UniqueSequence"_= i + 1
							, "Value"_= fp64(i)
							, "OutdatedStatus"_= "Ok"
							, "OutdatedSeconds"_= fp64::fs_Inf()
							, "SnoozeUntil"_= NTime::CTime()
							, "SensorMetaData"_= _={}
						}
					)
				;
				ExpectedSensorReadings.f_Array().f_Insert
					(
						_=
						{
							"HostID"_= ""
							, "HostName"_= ""
							, "Application"_= ""
							, "Identifier"_= "org.malterlib.testapp.test.version"
							, "IdentifierScope"_= ""
							, "Name"_= "Test Sensor (version)"
							, "UniqueSequence"_= i + 1
							, "Value"_= CEJsonUserTypeSorted
							{
								"Version"
								,
								{
									"Identifier"_j= "Ident",
									"Major"_j= 13,
									"Minor"_j= 3,
									"Revision"_j= i
								}
							}
							, "OutdatedStatus"_= "Ok"
							, "OutdatedSeconds"_= fp64::fs_Inf()
							, "SnoozeUntil"_= NTime::CTime()
							, "SensorMetaData"_= _={}
						}
					)
				;
			}

			CEJsonSorted ExpectedSensorStatus = _
				[
					_=
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.testapp.test"
						, "IdentifierScope"_= ""
						, "Name"_= "Test Sensor"
						, "UniqueSequence"_= 5
						, "Value"_= fp64(4.0)
						, "OutdatedStatus"_= "Ok"
						, "OutdatedSeconds"_= fp64::fs_Inf()
						, "SnoozeUntil"_= NTime::CTime()
						, "SensorMetaData"_= _={}
					}
					,
					_=
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.testapp.test.version"
						, "IdentifierScope"_= ""
						, "Name"_= "Test Sensor (version)"
						, "UniqueSequence"_= 5
						, "Value"_= CEJsonUserTypeSorted
						{
							"Version"
							,
							{
								"Identifier"_j= "Ident",
								"Major"_j= 13,
								"Minor"_j= 3,
								"Revision"_j= 4
							}
						}
						, "OutdatedStatus"_= "Ok"
						, "OutdatedSeconds"_= fp64::fs_Inf()
						, "SnoozeUntil"_= NTime::CTime()
						, "SensorMetaData"_= _={}
					}
				]
			;

			auto fSortSensors = [&](CEJsonSorted const &_Sensors)
				{
					CEJsonSorted Return = _Sensors;
					Return.f_Array().f_Sort
						(
							[](CEJsonSorted const &_Left, CEJsonSorted const &_Right)
							{
								auto fTuple = [](CEJsonSorted const &_Value)
									{
										return fg_TupleReferences
											(
												_Value["HostID"].f_String()
												, _Value["Application"].f_String()
												, _Value["Identifier"].f_String()
												, _Value["IdentifierScope"].f_String()
											)
										;
									}
								;
								return fTuple(_Left) <=> fTuple(_Right);
							}
						)
					;

					return Return;
				}
			;

			{
				DMibTestPath("Local Store");

				auto fGenerateSensorReading = g_ActorFunctor / [&]() -> TCFuture<void>
					{
						co_await AppManagerTestHelper.f_LaunchTool(TestAppDirectory / "TestApp", {"--generate-sensor-readings", "--no-random-values", "5"}, TestAppDirectory);
						co_await AppManagerTestHelper.f_LaunchTool
							(
								TestAppDirectory / "TestApp"
								, {"--generate-sensor-readings", "--no-random-values", "--reading-type=Version", "5"}
								, TestAppDirectory
							)
						;

						co_return {};
					}
				;

				co_await fGenerateSensorReading();

				auto fReadSensors = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						co_return fg_FromString(co_await AppManagerTestHelper.f_LaunchTool(TestAppDirectory / "TestApp", {"--sensor-list", "--json"}, TestAppDirectory));
					}
				;

				auto fReadSensorReadings = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						co_return fg_FromString(co_await AppManagerTestHelper.f_LaunchTool(TestAppDirectory / "TestApp", {"--sensor-readings-list", "--newest", "--json"}, TestAppDirectory));
					}
				;

				auto fReadSensorStatus = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						co_return fg_FromString
							(
								co_await AppManagerTestHelper.f_LaunchTool(TestAppDirectory / "TestApp", {"--sensor-status", "--no-only-problems", "--json"}, TestAppDirectory)
							)
						;
					}
				;

				DMibExpect(co_await fReadSensors(), ==, fSortSensors(ExpectedSensors));
				DMibExpect(fMakeComparable(co_await fReadSensorReadings()), ==, ExpectedSensorReadings);
				DMibExpect(fMakeComparable(co_await fReadSensorStatus()), ==, ExpectedSensorStatus);
			}

			auto TestAppHostID = CStr(co_await AppManagerTestHelper.f_LaunchTool(TestAppDirectory / "TestApp", {"--trust-host-id"}, TestAppDirectory)).f_Trim();
			auto HostName = CStr("{}@{}/TestApp"_f << NProcess::NPlatform::fg_Process_GetUserName() << NProcess::NPlatform::fg_Process_GetComputerName());
			auto fSetHostInfo = [&]
				(CEJsonSorted const &_Json)
				{
					CEJsonSorted Return = _Json;

					for (auto &Entry : Return.f_Array())
					{
						Entry["HostID"] = TestAppHostID;
						Entry["HostName"] = HostName;
						Entry["Application"] = "TestApp";
					}

					return Return;
				}
			;

			{
				DMibTestPath("Local Store AppManager");

				CStr AppManagerPath = AppManagerInfo.m_RootDirectory / "AppManager";

				auto fReadSensors = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						co_return fg_FromString(co_await AppManagerTestHelper.f_LaunchTool(AppManagerPath, {"--sensor-list", "--json"}, AppManagerInfo.m_RootDirectory));
					}
				;

				auto fReadSensorReadings = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						co_return fg_FromString(co_await AppManagerTestHelper.f_LaunchTool(AppManagerPath, {"--sensor-readings-list", "--newest", "--json"}, AppManagerInfo.m_RootDirectory));
					}
				;

				auto fReadSensorStatus = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						co_return fg_FromString
							(
								co_await AppManagerTestHelper.f_LaunchTool(AppManagerPath, {"--sensor-status", "--no-only-problems", "--json"}, AppManagerInfo.m_RootDirectory)
							)
						;
					}
				;

				DMibExpect(co_await fReadSensors(), ==, fSortSensors(fSetHostInfo(ExpectedSensors)));
				DMibExpect(fMakeComparable(co_await fReadSensorReadings()), ==, fSetHostInfo(ExpectedSensorReadings));
				DMibExpect(fSortSensors(fMakeComparable(co_await fReadSensorStatus())), ==, fSortSensors(fSetHostInfo(ExpectedSensorStatus)));
			}

			{
				DMibTestPath("Cloud Manager");

				CStr CloudClientDirectory = AppManagerTestHelper.m_pState->m_RootDirectory / "MalterlibCloud";
				CStr CloudClientPath = CloudClientDirectory / "MalterlibCloud";

				auto fReadSensors = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						co_return fg_FromString(co_await AppManagerTestHelper.f_LaunchTool(CloudClientPath, {"--cloud-manager-sensor-list", "--json"}, CloudClientDirectory));
					}
				;

				auto fReadSensorReadings = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						co_return fg_FromString
							(
								co_await AppManagerTestHelper.f_LaunchTool(CloudClientPath, {"--cloud-manager-sensor-readings-list", "--newest", "--json"}, CloudClientDirectory)
							)
						;
					}
				;

				auto fReadSensorStatus = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						co_return fg_FromString
							(
								co_await AppManagerTestHelper.f_LaunchTool(CloudClientPath, {"--cloud-manager-sensor-status", "--no-only-problems", "--json"}, CloudClientDirectory)
							)
						;
					}
				;

				DMibExpect(co_await fReadSensors(), ==, fSortSensors(fSetHostInfo(ExpectedSensors)));
				DMibExpect
					(
						fMakeComparable(co_await fReadSensorReadings())
						, ==
						, fSetHostInfo(ExpectedSensorReadings)
					)
				;
				DMibExpect
					(
						fSortSensors(fMakeComparable(co_await fReadSensorStatus()))
						, ==
						, fSortSensors(fSetHostInfo(ExpectedSensorStatus))
					)
				;
			}

			co_return {};
		};
	}
};

DMibTestRegister(CAppManager_Sensor_Tests, Malterlib::Cloud);
