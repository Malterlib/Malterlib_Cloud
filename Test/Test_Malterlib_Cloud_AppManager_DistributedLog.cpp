// Copyright © 2022 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Test_Malterlib_Cloud_AppManager.h"

#ifdef DPlatformFamily_Windows
#include <Windows.h>
#endif

static fp64 g_Timeout = 120.0 * NMib::NTest::gc_TimeoutMultiplier;

class CAppManager_DistributedLog_Tests : public NMib::NTest::CTest
{
public:
	void f_DoTests()
	{
		DMibTestSuite("General") -> TCFuture<void>
		{
			CAppManagerTestHelper::EOption Options = CAppManagerTestHelper::EOption_LaunchTestAppInApp | CAppManagerTestHelper::EOption_EnableVersionManager;
			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("AppManagerDistributedLogTests", Options, g_Timeout);
			co_await AppManagerTestHelper.f_Setup(1);
			auto &AppManagerInfo = *AppManagerTestHelper.m_AppManagerInfos.f_FindAny();

			auto fMakeComparable = [](CEJSON &&_Entries)
				{
					for (auto &Entry : _Entries.f_Array())
					{
						Entry.f_RemoveMember("Timestamp");
						Entry.f_RemoveMember("Value");
					}

					// We need to sort by sequence because the system clock might change
					_Entries.f_Array().f_Sort
						(
							[](CEJSON const &_Left, CEJSON const &_Right)
							{
								auto Left = _Left.f_GetMemberValue("UniqueSequence", 0);
								auto Right = _Right.f_GetMemberValue("UniqueSequence", 0);

								return Left < Right;
							}
						)
					;

					return fg_Move(_Entries);
				}
			;

			auto TestAppDirectory = AppManagerInfo.m_RootDirectory / "App/TestApp";

			CEJSON ExpectedLogs = CEJSON
				{
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.log.test"
						, "IdentifierScope"_= "Test"
						, "Name"_= "Malterlib Test"
					}
				}
			;

			CEJSON ExpectedLogEntries = CEJSON
				{
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.log.test"
						, "IdentifierScope"_= "Test"
						, "Name"_= "Malterlib Test"
						, "UniqueSequence"_= 1
						, "Data"_=
						{
							"Message"_= "Test Log 0"
							, "Categories"_= _[_]
							, "Operations"_= _[_]
							, "Severity"_= "Info"
							, "Flags"_= _[_]
						}
					}
					,
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.log.test"
						, "IdentifierScope"_= "Test"
						, "Name"_= "Malterlib Test"
						, "UniqueSequence"_= 2
						, "Data"_=
						{
							"Message"_= "Test Log 1"
							, "Categories"_= _[_]
							, "Operations"_= _[_]
							, "Severity"_= "Info"
							, "Flags"_= _[_]
						}
					}
					,
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.log.test"
						, "IdentifierScope"_= "Test"
						, "Name"_= "Malterlib Test"
						, "UniqueSequence"_= 3
						, "Data"_=
						{
							"Message"_= "Test Log 2"
							, "Categories"_= _[_]
							, "Operations"_= _[_]
							, "Severity"_= "Info"
							, "Flags"_= _[_]
						}
					}
					,
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.log.test"
						, "IdentifierScope"_= "Test"
						, "Name"_= "Malterlib Test"
						, "UniqueSequence"_= 4
						, "Data"_=
						{
							"Message"_= "Test Log 3"
							, "Categories"_= _[_]
							, "Operations"_= _[_]
							, "Severity"_= "Info"
							, "Flags"_= _[_]
						}
					}
					,
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.log.test"
						, "IdentifierScope"_= "Test"
						, "Name"_= "Malterlib Test"
						, "UniqueSequence"_= 5
						, "Data"_=
						{
							"Message"_= "Test Log 4"
							, "Categories"_= _[_]
							, "Operations"_= _[_]
							, "Severity"_= "Info"
							, "Flags"_= _[_]
						}
					}
				}
			;

			auto fSortLogs = [&](CEJSON const &_Logs)
				{
					CEJSON Return = _Logs;
					Return.f_Array().f_Sort
						(
							[](CEJSON const &_Left, CEJSON const &_Right) -> bool
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
								return fTuple(_Left) < fTuple(_Right);
							}
						)
					;

					return Return;
				}
			;

			{
				DMibTestPath("Local Store");

				auto fGenerateLogEntry = [&]()
					{
						CProcessLaunch::fs_LaunchTool(TestAppDirectory / "TestApp", {"--generate-log-entries", "5"}, TestAppDirectory);
					}
				;

				fGenerateLogEntry();

				auto fReadLogs = [&]()
					{
						return CEJSON::fs_FromString(CProcessLaunch::fs_LaunchTool(TestAppDirectory / "TestApp", {"--log-list", "--json"}, TestAppDirectory));
					}
				;

				auto fReadLogEntries = [&]()
					{
						return CEJSON::fs_FromString
							(
								CProcessLaunch::fs_LaunchTool(TestAppDirectory / "TestApp", {"--log-entries-list", "--newest", "--json"}, TestAppDirectory)
							)
						;
					}
				;

				DMibExpect(fReadLogs(), ==, fSortLogs(ExpectedLogs));
				DMibExpect(fMakeComparable(fReadLogEntries()), ==, ExpectedLogEntries);
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

				auto fReadLogs = [&]()
					{
						return CEJSON::fs_FromString(CProcessLaunch::fs_LaunchTool(AppManagerPath, {"--log-list", "--json"}, AppManagerInfo.m_RootDirectory));
					}
				;

				auto fReadLogEntries = [&]()
					{
						return CEJSON::fs_FromString
							(
								CProcessLaunch::fs_LaunchTool(AppManagerPath, {"--log-entries-list", "--newest", "--json"}, AppManagerInfo.m_RootDirectory)
							)
						;
					}
				;

				DMibExpect(fReadLogs(), ==, fSortLogs(fSetHostInfo(ExpectedLogs)));
				DMibExpect(fMakeComparable(fReadLogEntries()), ==, fSetHostInfo(ExpectedLogEntries));
			}

			auto fSetHostInfoAppManager =
				[
					&
					, AppManagerHostID = AppManagerInfo.f_GetHostID()
					, HostName = CStr("{}@{}/AppManager"_f << NProcess::NPlatform::fg_Process_GetUserName() << NProcess::NPlatform::fg_Process_GetComputerName())
				]
				(CEJSON const &_JSON)
				{
					CEJSON Return = _JSON;

					for (auto &Entry : Return.f_Array())
					{
						Entry["HostID"] = AppManagerHostID;
						Entry["HostName"] = HostName;
					}

					return Return;
				}
			;

			{
				DMibTestPath("Cloud Manager");

				CStr CloudClientDirectory = AppManagerTestHelper.m_RootDirectory / "MalterlibCloud";
				CStr CloudClientPath = CloudClientDirectory / "MalterlibCloud";

				auto fReadLogs = [&]()
					{
						return CEJSON::fs_FromString
							(
								CProcessLaunch::fs_LaunchTool(CloudClientPath, {"--cloud-manager-log-list", "--json"}, CloudClientDirectory)
							)
						;
					}
				;

				auto fReadLogEntries = [&]()
					{
						return CEJSON::fs_FromString
							(
								CProcessLaunch::fs_LaunchTool(CloudClientPath, {"--cloud-manager-log-entries-list", "--newest", "--json"}, CloudClientDirectory)
							)
						;
					}
				;

				DMibExpect(fReadLogs(), ==, fSortLogs(fSetHostInfo(ExpectedLogs)));
				DMibExpect
					(
						fMakeComparable(fReadLogEntries())
						, ==
						, fSetHostInfo(ExpectedLogEntries)
					)
				;
			}

			co_return {};
		};
	}
};

DMibTestRegister(CAppManager_DistributedLog_Tests, Malterlib::Cloud);
