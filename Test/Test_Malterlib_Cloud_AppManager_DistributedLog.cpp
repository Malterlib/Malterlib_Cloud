// Copyright © 2022 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Test_Malterlib_Cloud_AppManager.h"

#ifdef DPlatformFamily_Windows
#	include <Windows.h>
#endif

static fp64 g_Timeout = 120.0 * NMib::NTest::gc_TimeoutMultiplier;

class CAppManager_DistributedLog_Tests : public NMib::NTest::CTest
{
public:
	void fp_TestGeneral()
	{
		DMibTestSuite("General") -> TCFuture<void>
		{
			CAppManagerTestHelper::EOption Options 
				= CAppManagerTestHelper::EOption_LaunchTestAppInApp
				| CAppManagerTestHelper::EOption_EnableVersionManager
				| CAppManagerTestHelper::EOption_DisablePatchMonitoring
			;

			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("AppManagerDistributedLogTests", Options, g_Timeout);

			auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

			co_await AppManagerTestHelper.f_Setup(1);
			auto &AppManagerInfo = *AppManagerTestHelper.m_pState->m_AppManagerInfos.f_FindAny();

			auto fMakeComparable = [](CEJsonSorted &&_Entries)
				{
					for (auto &Entry : _Entries.f_Array())
					{
						Entry.f_RemoveMember("Timestamp");
						Entry.f_RemoveMember("Value");
					}

					// We need to sort by sequence because the system clock might change
					_Entries.f_Array().f_Sort
						(
							[](CEJsonSorted const &_Left, CEJsonSorted const &_Right)
							{
								auto Left = _Left.f_GetMemberValue("UniqueSequence", 0);
								auto Right = _Right.f_GetMemberValue("UniqueSequence", 0);

								return Left <=> Right;
							}
						)
					;

					return fg_Move(_Entries);
				}
			;

			auto TestAppDirectory = AppManagerInfo.m_RootDirectory / "App/TestApp";

			CEJsonSorted ExpectedLogs = _
				[
					_=
					{
						"HostID"_= ""
						, "HostName"_= ""
						, "Application"_= ""
						, "Identifier"_= "org.malterlib.log.test"
						, "IdentifierScope"_= "Test"
						, "Name"_= "Malterlib Test"
						, "Removed"_= false
						, "LogMetadata"_= _={}
					}
				]
			;

			CEJsonSorted ExpectedLogEntries = _
				[
					_=
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
							, "Categories"_= _[]
							, "Operations"_= _[]
							, "Severity"_= "Info"
							, "Flags"_= _[]
						}
						, "LogMetadata"_= _={}
					}
					, _=
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
							, "Categories"_= _[]
							, "Operations"_= _[]
							, "Severity"_= "Info"
							, "Flags"_= _[]
						}
						, "LogMetadata"_= _={}
					}
					, _=
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
							, "Categories"_= _[]
							, "Operations"_= _[]
							, "Severity"_= "Info"
							, "Flags"_= _[]
						}
						, "LogMetadata"_= _={}
					}
					, _=
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
							, "Categories"_= _[]
							, "Operations"_= _[]
							, "Severity"_= "Info"
							, "Flags"_= _[]
						}
						, "LogMetadata"_= _={}
					}
					, _=
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
							, "Categories"_= _[]
							, "Operations"_= _[]
							, "Severity"_= "Info"
							, "Flags"_= _[]
						}
						, "LogMetadata"_= _={}
					}
				]
			;

			auto fSortLogs = [&](CEJsonSorted const &_Logs)
				{
					CEJsonSorted Return = _Logs;
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

				auto fGenerateLogEntry = g_ActorFunctor / [&]() -> TCFuture<void>
					{
						co_await AppManagerTestHelper.f_LaunchTool(TestAppDirectory / "TestApp", {"--generate-log-entries", "5"}, TestAppDirectory);

						co_return {};
					}
				;

				co_await fGenerateLogEntry();

				auto fReadLogs = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						auto Result = co_await AppManagerTestHelper.f_LaunchTool
							(
								TestAppDirectory / "TestApp"
								, {"--log-list", "--identifier", "org.malterlib.log.test", "--json"}, TestAppDirectory
							)
						;
						co_return CEJsonSorted::fs_FromString(Result);
					}
				;

				auto fReadLogEntries = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						auto Result = co_await AppManagerTestHelper.f_LaunchTool
							(
								TestAppDirectory / "TestApp"
								, {"--log-entries-list", "--identifier", "org.malterlib.log.test", "--newest", "--max-entries=0", "--json"}
								, TestAppDirectory
							)
						;

						co_return CEJsonSorted::fs_FromString(Result);
					}
				;

				DMibExpect(co_await fReadLogs(), ==, fSortLogs(ExpectedLogs));
				DMibExpect(fMakeComparable(co_await fReadLogEntries()), ==, ExpectedLogEntries);
			}

			auto TestAppHostID = CStr((co_await AppManagerTestHelper.f_LaunchTool(TestAppDirectory / "TestApp", {"--trust-host-id"}, TestAppDirectory)).f_Trim());
			auto HostName = CStr("{}@{}/TestApp"_f << NProcess::NPlatform::fg_Process_GetUserName() << NProcess::NPlatform::fg_Process_GetComputerName());
			auto fSetHostInfo = [&](CEJsonSorted const &_Json)
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

				auto fReadLogs = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						auto Result = co_await AppManagerTestHelper.f_LaunchTool
							(
								AppManagerPath
								, {"--log-list", "--identifier", "org.malterlib.log.test", "--json"}
								, AppManagerInfo.m_RootDirectory
							)
						;
						co_return CEJsonSorted::fs_FromString(Result);
					}
				;

				auto fReadLogEntries = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						auto Result = co_await AppManagerTestHelper.f_LaunchTool
							(
								AppManagerPath
								, {"--log-entries-list", "--identifier", "org.malterlib.log.test", "--newest", "--max-entries=0", "--json"}
								, AppManagerInfo.m_RootDirectory
							)
						;
						co_return CEJsonSorted::fs_FromString(Result);
					}
				;

				DMibExpect(co_await fReadLogs(), ==, fSortLogs(fSetHostInfo(ExpectedLogs)));
				DMibExpect(fMakeComparable(co_await fReadLogEntries()), ==, fSetHostInfo(ExpectedLogEntries));
			}

			auto fSetHostInfoAppManager =
				[
					&
					, AppManagerHostID = AppManagerInfo.f_GetHostID()
					, HostName = CStr("{}@{}/AppManager"_f << NProcess::NPlatform::fg_Process_GetUserName() << NProcess::NPlatform::fg_Process_GetComputerName())
				]
				(CEJsonSorted const &_Json)
				{
					CEJsonSorted Return = _Json;

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

				CStr CloudClientDirectory = AppManagerTestHelper.m_pState->m_RootDirectory / "MalterlibCloud";
				CStr CloudClientPath = CloudClientDirectory / "MalterlibCloud";

				auto fReadLogs = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						auto Result = co_await AppManagerTestHelper.f_LaunchTool
							(
								CloudClientPath
								, {"--cloud-manager-log-list", "--identifier", "org.malterlib.log.test", "--json"}
								, CloudClientDirectory
							)
						;
						co_return CEJsonSorted::fs_FromString(Result);
					}
				;

				auto fReadLogEntries = g_ActorFunctor / [&]() -> TCFuture<CEJsonSorted>
					{
						auto Result = co_await AppManagerTestHelper.f_LaunchTool
							(
								CloudClientPath
								, {"--cloud-manager-log-entries-list", "--identifier", "org.malterlib.log.test", "--newest", "--max-entries=0", "--json"}
								, CloudClientDirectory
							)
						;
						co_return CEJsonSorted::fs_FromString(Result);
					}
				;

				DMibExpect(co_await fReadLogs(), ==, fSortLogs(fSetHostInfo(ExpectedLogs)));
				DMibExpect
					(
						fMakeComparable(co_await fReadLogEntries())
						, ==
						, fSetHostInfo(ExpectedLogEntries)
					)
				;
			}

			co_return {};
		};
	}

	TCFuture<void> fp_TestLimitsImpl
		(
			CStr _Name
			, TCSharedPointer<TCActorFunctor<TCFuture<void> (CStr _Root, CAppManagerTestHelper *_pAppManagerTestHelper)>> _pGenerateEntries
			, TCFunction<void (CEJsonSorted const &_Entries)> _fCheckEntries, bool _bStopCloudManager
		)
	{
		CAppManagerTestHelper::EOption Options = CAppManagerTestHelper::EOption_LaunchTestAppInApp
			| CAppManagerTestHelper::EOption_EnableVersionManager 
			| CAppManagerTestHelper::EOption_DisablePatchMonitoring
			| CAppManagerTestHelper::EOption_DisableDiskMonitoring
			| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
			| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
		;

		if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
			Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

		CAppManagerTestHelper AppManagerTestHelper("AppManagerDistributedLogTests{}"_f << _Name, Options, g_Timeout);
		auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

		co_await AppManagerTestHelper.f_Setup(1);
		auto &AppManagerInfo = *AppManagerTestHelper.m_pState->m_AppManagerInfos.f_FindAny();

		auto TestAppDirectory = AppManagerInfo.m_RootDirectory / "App/TestApp";

		if (_bStopCloudManager)
			co_await AppManagerTestHelper.f_StopCloudManager();

		{
			DMibTestPath("Local Store");

			co_await (*_pGenerateEntries)(TestAppDirectory, &AppManagerTestHelper);

			auto LogEntries = CEJsonSorted::fs_FromString
				(
					co_await AppManagerTestHelper.f_LaunchTool
					(
						TestAppDirectory / "TestApp"
						, {"--log-entries-list", "--identifier", "org.malterlib.log.test", "--newest", "--max-entries=0", "--json"}
						, TestAppDirectory
					)
				)
			;
			_fCheckEntries(LogEntries);
		}

		{
			DMibTestPath("Local Store AppManager");

			CStr AppManagerPath = AppManagerInfo.m_RootDirectory / "AppManager";

			auto LogEntries = CEJsonSorted::fs_FromString
				(
					co_await AppManagerTestHelper.f_LaunchTool
					(
						AppManagerPath
						, {"--log-entries-list", "--identifier", "org.malterlib.log.test", "--newest", "--max-entries=0", "--json"}
						, AppManagerInfo.m_RootDirectory
					)
				)
			;
			_fCheckEntries(LogEntries);
		}

		if (_bStopCloudManager)
		{
			co_await AppManagerTestHelper.f_StartCloudManager();
			NTime::CClock Clock{true};
			while (true)
			{
				try
				{
					auto ReportDepth =
						(
							co_await AppManagerTestHelper.f_LaunchTool
							(
								TestAppDirectory / "TestApp"
								, {"--get-log-report-depth"}
								, TestAppDirectory
							)
						).f_Trim().f_ToInt(uint32(0))
					;

					if (ReportDepth == 3)
						break;
				}
				catch (NException::CException const &)
				{
				}

				co_await fg_Timeout(0.1);

				if (Clock.f_GetTime() > g_Timeout)
					co_return DMibErrorInstance("Timeout waiting for cloud manager to be fully started");
			}
		}

		{
			DMibTestPath("Cloud Manager");

			CStr CloudClientDirectory = AppManagerTestHelper.m_pState->m_RootDirectory / "MalterlibCloud";
			CStr CloudClientPath = CloudClientDirectory / "MalterlibCloud";

			auto LogEntries = CEJsonSorted::fs_FromString
				(
					co_await AppManagerTestHelper.f_LaunchTool
					(
						CloudClientPath
						, {"--cloud-manager-log-entries-list", "--identifier", "org.malterlib.log.test", "--newest", "--max-entries=0", "--json"}
						, CloudClientDirectory
					)
				)
			;
			_fCheckEntries(LogEntries);
		}

		co_return {};
	}

	TCFuture<void> fp_TestLimitsImpl
		(
			CStr _Name
			, TCActorFunctor<TCFuture<void> (CStr _Root, CAppManagerTestHelper *_pAppManagerTestHelper)> _fGenerateEntries
			, TCFunction<void (CEJsonSorted const &_Entries)> _fCheckEntries
		)
	{
		TCSharedPointer<TCActorFunctor<TCFuture<void> (CStr _Root, CAppManagerTestHelper *_pAppManagerTestHelper)>> pGenerateEntries = fg_Construct(fg_Move(_fGenerateEntries));
		{
			DMibTestPath("Started CloudManager");
			co_await fp_TestLimitsImpl(_Name + "CMStarted", pGenerateEntries, _fCheckEntries, false);
		}
		{
			DMibTestPath("Stopped CloudManager");
			co_await fp_TestLimitsImpl(_Name + "CMStopped", pGenerateEntries, _fCheckEntries, true);
		}

		co_return {};
	}

	void fp_CheckLimitedEntries(CEJsonSorted const &_Entries, mint _LineLen, mint _TotalSize)
	{
		CStr AllEntries;
		for (auto &LogEntry : _Entries.f_Array())
			AllEntries += LogEntry["Data"]["Message"].f_String();

		bool bAllLinesOk = true;
		for (auto &Line : AllEntries.f_SplitLine())
		{
			if (Line.f_IsEmpty())
				continue;

			if (Line != CStr("<{sj*,sf }>"_f << "" << (_LineLen - 3)))
				bAllLinesOk = false;
		}

		DMibExpectTrue(bAllLinesOk);
		DMibExpect(AllEntries.f_GetLen(), ==, _TotalSize);
	}

	void fp_TestLimits()
	{
		DMibTestSuite("Big Entry") -> TCFuture<void>
		{
			mint EntrySize = CActorDistributionManager::mc_MaxMessageSize * 2;
			mint LineSize = 256;

			co_return co_await fp_TestLimitsImpl
				(
					"BigEntry"
					, g_ActorFunctor / [&](CStr _Root, CAppManagerTestHelper *_pAppManagerTestHelper) -> TCFuture<void>
					{
						co_await _pAppManagerTestHelper->f_LaunchTool
							(
								_Root / "TestApp"
								, {"--generate-huge-log-entries", "--num-entries=1", "--line-size={}"_f << LineSize, "--entry-size={}"_f << EntrySize}
								, _Root
							)
						;

						co_return {};
					}
					, [&](CEJsonSorted const &_Entries)
					{
						fp_CheckLimitedEntries(_Entries, LineSize, EntrySize);
					}
				)
			;
		};
		DMibTestSuite("Big Line") -> TCFuture<void>
		{
			mint EntrySize = CActorDistributionManager::mc_MaxMessageSize * 2;
			mint LineSize = EntrySize;

			co_return co_await fp_TestLimitsImpl
				(
					"BigLine"
					, g_ActorFunctor / [&](CStr _Root, CAppManagerTestHelper *_pAppManagerTestHelper) -> TCFuture<void>
					{
						co_await _pAppManagerTestHelper->f_LaunchTool
							(
								_Root / "TestApp"
								, {"--generate-huge-log-entries", "--num-entries=1", "--line-size={}"_f << LineSize, "--entry-size={}"_f << EntrySize}
								, _Root
							)
						;

						co_return {};
					}
					, [&](CEJsonSorted const &_Entries)
					{
						fp_CheckLimitedEntries(_Entries, LineSize, EntrySize);
					}
				)
			;
		};
		DMibTestSuite("Many Entries") -> TCFuture<void>
		{
			mint TotalSize = CActorDistributionManager::mc_MaxMessageSize * 2;
			mint nEntries = 256;
			mint EntrySize = TotalSize / nEntries;
			mint LineSize = 256;

			co_return co_await fp_TestLimitsImpl
				(
					"ManyLines"
					, g_ActorFunctor / [&](CStr _Root, CAppManagerTestHelper *_pAppManagerTestHelper) -> TCFuture<void>
					{
						co_await _pAppManagerTestHelper->f_LaunchTool
							(
								_Root / "TestApp"
								, {"--generate-huge-log-entries", "--num-entries={}"_f << nEntries, "--line-size={}"_f << LineSize, "--entry-size={}"_f << EntrySize}
								, _Root
							)
						;

						co_return {};
					}
					, [&](CEJsonSorted const &_Entries)
					{
						fp_CheckLimitedEntries(_Entries, LineSize, TotalSize);
					}
				)
			;
		};
	}

	void f_DoTests()
	{
		fp_TestGeneral();
		fp_TestLimits();
	}
};

DMibTestRegister(CAppManager_DistributedLog_Tests, Malterlib::Cloud);
