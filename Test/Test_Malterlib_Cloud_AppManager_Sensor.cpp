// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Test_Malterlib_Cloud_AppManager.h"

#ifdef DPlatformFamily_Windows
#include <Windows.h>
#endif

static fp64 g_Timeout = 120.0 * NMib::NTest::gc_TimeoutMultiplier;

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
			;

			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("AppManagerSensorTests", Options, g_Timeout);

			auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

			co_await AppManagerTestHelper.f_Setup(1);
			auto &AppManagerInfo = *AppManagerTestHelper.m_pState->m_AppManagerInfos.f_FindAny();
			auto fShouldKeepValue = [](CEJSON const &_Reading)
				{
					return _Reading["Identifier"].f_String().f_StartsWith("org.malterlib.testapp.test");
				}
			;

			auto fMakeComparable = [&](CEJSON &&_Readings)
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
							[](CEJSON const &_Left, CEJSON const &_Right)
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

			CEJSON ExpectedSensors = CEJSON
				{
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.testapp.test.version"
						, "IdentifierScope"_= ""
						, "Name"_= "Test Sensor (version)"
						, "Type"_= 5
						, "ExpectedReportInterval"_= nullptr
						, "UnitDivisors"_= _[_]
						, "WarnValue"_= nullptr
						, "CriticalValue"_= nullptr
						, "Removed"_= false
					}
					,
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.testapp.test"
						, "IdentifierScope"_= ""
						, "Name"_= "Test Sensor"
						, "Type"_= 2
						, "ExpectedReportInterval"_= nullptr
						, "UnitDivisors"_= CEJSON
						{
							{
								"Divisor"_= 0.0
								, "nDecimals"_= 1
								, "UnitFormatter"_= "{fn0} B"
							}
							,
							{
								"Divisor"_= 1024.0
								, "nDecimals"_= 1
								, "UnitFormatter"_= "{fn1} KiB"
							}
							,
							{
								"Divisor"_= 1048576.0
								, "nDecimals"_= 1
								, "UnitFormatter"_= "{fn1} MiB"
							}
							,
							{
								"Divisor"_= 1073741824.0
								, "nDecimals"_= 1
								, "UnitFormatter"_= "{fn1} GiB"
							}
							,
							{
								"Divisor"_= 1099511627776.0
								, "nDecimals"_= 1
								, "UnitFormatter"_= "{fn1} TiB"
							}
						}
						, "WarnValue"_= nullptr
						, "CriticalValue"_= nullptr
						, "Removed"_= false
					}
				}
			;

			CEJSON ExpectedSensorReadings;
			for (mint i = 0; i < 5; ++i)
			{
				ExpectedSensorReadings.f_Array().f_Insert
					(
						{
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
								, "OutdatedSeconds"_= nullptr
							}
						}
					)
				;
				ExpectedSensorReadings.f_Array().f_Insert
					(
						{
							{
								"HostID"_= ""
								, "HostName"_= ""
								, "Application"_= ""
								, "Identifier"_= "org.malterlib.testapp.test.version"
								, "IdentifierScope"_= ""
								, "Name"_= "Test Sensor (version)"
								, "UniqueSequence"_= i + 1
								, "Value"_= CEJSONUserType
								{
									"Version"
									,
									{
										"Identifier"__= "Ident",
										"Major"__= 13,
										"Minor"__= 3,
										"Revision"__= i
									}
								}
								, "OutdatedStatus"_= "Ok"
								, "OutdatedSeconds"_= nullptr
							}
						}
					)
				;
			}

			CEJSON ExpectedSensorStatus = CEJSON
				{
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
						, "OutdatedSeconds"_= nullptr
					}
					,
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.testapp.test.version"
						, "IdentifierScope"_= ""
						, "Name"_= "Test Sensor (version)"
						, "UniqueSequence"_= 5
						, "Value"_= CEJSONUserType
						{
							"Version"
							,
							{
								"Identifier"__= "Ident",
								"Major"__= 13,
								"Minor"__= 3,
								"Revision"__= 4
							}
						}
						, "OutdatedStatus"_= "Ok"
						, "OutdatedSeconds"_= nullptr
					},
				}
			;

			auto fSortSensors = [&](CEJSON const &_Sensors)
				{
					CEJSON Return = _Sensors;
					Return.f_Array().f_Sort
						(
							[](CEJSON const &_Left, CEJSON const &_Right)
							{
								auto fTuple = [](CEJSON const &_Value)
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

				auto fGenerateSensorReading = [&]()
					{
						CProcessLaunch::fs_LaunchTool(TestAppDirectory / "TestApp", {"--generate-sensor-readings", "--no-random-values", "5"}, TestAppDirectory);
						CProcessLaunch::fs_LaunchTool(TestAppDirectory / "TestApp", {"--generate-sensor-readings", "--no-random-values", "--reading-type=Version", "5"}, TestAppDirectory);
					}
				;

				fGenerateSensorReading();

				auto fReadSensors = [&]()
					{
						return CEJSON::fs_FromString(CProcessLaunch::fs_LaunchTool(TestAppDirectory / "TestApp", {"--sensor-list", "--json"}, TestAppDirectory));
					}
				;

				auto fReadSensorReadings = [&]()
					{
						return CEJSON::fs_FromString
							(
								CProcessLaunch::fs_LaunchTool(TestAppDirectory / "TestApp", {"--sensor-readings-list", "--newest", "--json"}, TestAppDirectory)
							)
						;
					}
				;

				auto fReadSensorStatus = [&]()
					{
						return CEJSON::fs_FromString(CProcessLaunch::fs_LaunchTool(TestAppDirectory / "TestApp", {"--sensor-status", "--no-only-problems", "--json"}, TestAppDirectory));
					}
				;

				DMibExpect(fReadSensors(), ==, fSortSensors(ExpectedSensors));
				DMibExpect(fMakeComparable(fReadSensorReadings()), ==, ExpectedSensorReadings);
				DMibExpect(fMakeComparable(fReadSensorStatus()), ==, ExpectedSensorStatus);
			}

			auto fSetHostInfo =
				[
					&
					, TestAppHostID = CStr(CProcessLaunch::fs_LaunchTool(TestAppDirectory / "TestApp", {"--trust-host-id"}, TestAppDirectory).f_Trim())
					, HostName = CStr("{}@{}/TestApp"_f << NProcess::NPlatform::fg_Process_GetUserName() << NProcess::NPlatform::fg_Process_GetComputerName())
				]
				(CEJSON const &_JSON)
				{
					CEJSON Return = _JSON;

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

				auto fReadSensors = [&]()
					{
						return CEJSON::fs_FromString(CProcessLaunch::fs_LaunchTool(AppManagerPath, {"--sensor-list", "--json"}, AppManagerInfo.m_RootDirectory));
					}
				;

				auto fReadSensorReadings = [&]()
					{
						return CEJSON::fs_FromString
							(
								CProcessLaunch::fs_LaunchTool(AppManagerPath, {"--sensor-readings-list", "--newest", "--json"}, AppManagerInfo.m_RootDirectory)
							)
						;
					}
				;

				auto fReadSensorStatus = [&]()
					{
						return CEJSON::fs_FromString(CProcessLaunch::fs_LaunchTool(AppManagerPath, {"--sensor-status", "--no-only-problems", "--json"}, AppManagerInfo.m_RootDirectory));
					}
				;

				DMibExpect(fReadSensors(), ==, fSortSensors(fSetHostInfo(ExpectedSensors)));
				DMibExpect(fMakeComparable(fReadSensorReadings()), ==, fSetHostInfo(ExpectedSensorReadings));
				DMibExpect(fSortSensors(fMakeComparable(fReadSensorStatus())), ==, fSortSensors(fSetHostInfo(ExpectedSensorStatus)));
			}

			{
				DMibTestPath("Cloud Manager");

				CStr CloudClientDirectory = AppManagerTestHelper.m_pState->m_RootDirectory / "MalterlibCloud";
				CStr CloudClientPath = CloudClientDirectory / "MalterlibCloud";

				auto fReadSensors = [&]()
					{
						return CEJSON::fs_FromString
							(
								CProcessLaunch::fs_LaunchTool(CloudClientPath, {"--cloud-manager-sensor-list", "--json"}, CloudClientDirectory)
							)
						;
					}
				;

				auto fReadSensorReadings = [&]()
					{
						return CEJSON::fs_FromString
							(
								CProcessLaunch::fs_LaunchTool(CloudClientPath, {"--cloud-manager-sensor-readings-list", "--newest", "--json"}, CloudClientDirectory)
							)
						;
					}
				;

				auto fReadSensorStatus = [&]()
					{
						return CEJSON::fs_FromString
							(
								CProcessLaunch::fs_LaunchTool(CloudClientPath, {"--cloud-manager-sensor-status", "--no-only-problems", "--json"}, CloudClientDirectory)
							)
						;
					}
				;

				DMibExpect(fReadSensors(), ==, fSortSensors(fSetHostInfo(ExpectedSensors)));
				DMibExpect
					(
						fMakeComparable(fReadSensorReadings())
						, ==
						, fSetHostInfo(ExpectedSensorReadings)
					)
				;
				DMibExpect
					(
						fSortSensors(fMakeComparable(fReadSensorStatus()))
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
