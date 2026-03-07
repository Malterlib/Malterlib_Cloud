// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Test_Malterlib_Cloud_AppManager.h"

#include <Mib/Test/Exception>

#ifdef DPlatformFamily_Windows
#	include <Windows.h>
#endif

struct CRefreshResult
{
	static CRefreshResult fs_Parse(CStr const &_Output)
	{
		CRefreshResult Result;
		aint nParsed = 0;
		(CStr::CParse("Refreshed versions: {} added, {} updated, {} removed") >> Result.m_nAdded >> Result.m_nUpdated >> Result.m_nRemoved).f_Parse(_Output, nParsed);
		return Result;
	}

	mint m_nAdded = 0;
	mint m_nUpdated = 0;
	mint m_nRemoved = 0;
};

namespace
{
	fp64 g_Timeout = 120.0 * NMib::NTest::gc_TimeoutMultiplier;
}

struct CVersionManager_Refresh_Tests : public NMib::NTest::CTest
{
	void f_DoTests()
	{
		DMibTestSuite("Basic") -> TCFuture<void>
		{
			CAppManagerTestHelper::EOption Options
				= CAppManagerTestHelper::EOption_EnableVersionManager
				| CAppManagerTestHelper::EOption_DisablePatchMonitoring
				| CAppManagerTestHelper::EOption_DisableDiskMonitoring
				| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
				| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
			;

			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("VersionManagerRefreshTests_Basic", Options, g_Timeout);
			auto &State = *AppManagerTestHelper.m_pState;

			auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

			co_await AppManagerTestHelper.f_Setup(1);

			CStr VersionManagerPath = State.m_VersionManagerDirectory / "VersionManager";
			CStr VersionManagerDirectory = State.m_VersionManagerDirectory;

			auto fRefreshVersions = [&]() -> TCFuture<CRefreshResult>
				{
					CStr Output = co_await AppManagerTestHelper.f_LaunchTool(VersionManagerPath, {"--refresh-versions"}, VersionManagerDirectory);
					co_return CRefreshResult::fs_Parse(Output);
				}
			;

			auto fListVersions = [&](CStr _Application) -> TCFuture<CVersionManager::CListVersions::CResult>
				{
					CVersionManager::CListVersions Params;
					Params.m_ForApplication = _Application;
					co_return co_await State.m_VersionManager.f_CallActor(&CVersionManager::f_ListVersions)(Params)
						.f_Timeout(g_Timeout, "Timed out waiting for ListVersions")
					;
				}
			;

			auto fCreateVersionOnDisk = [&](CStr _App, CVersionManager::CVersionIDAndPlatform _VersionID, CVersionManager::CVersionInformation _Info) -> TCFuture<void>
				{
					auto BlockingActorCheckout = fg_BlockingActor();

					co_await
						(
							g_Dispatch(BlockingActorCheckout) / [=, VersionManagerDirectory = VersionManagerDirectory]()
							{
								CStr ApplicationDirectory = VersionManagerDirectory / "Applications" / _App;
								CFile::fs_CreateDirectory(ApplicationDirectory);

								CStr VersionPath = ApplicationDirectory / _VersionID.f_EncodeFileName();
								CFile::fs_CreateDirectory(VersionPath);

								// Create a dummy file so the version has content
								CFile::fs_WriteStringToFile(VersionPath / "dummy.txt", "test content");

								// Write version info JSON
								CStr VersionInfoPath = VersionPath + ".json";
								CEJsonSorted VersionInfo = EJsonType_Object;
								VersionInfo["Time"] = _Info.m_Time;
								VersionInfo["Configuration"] = _Info.m_Configuration;
								if (!_Info.m_Tags.f_IsEmpty())
								{
									CEJsonSorted Tags = EJsonType_Array;
									for (auto &Tag : _Info.m_Tags)
										Tags.f_Insert(_Info.m_Tags.fs_GetKey(Tag));
									VersionInfo["Tags"] = Tags;
								}
								CFile::fs_WriteStringToFile(VersionInfoPath, VersionInfo.f_ToString(nullptr));
							}
						)
					;

					co_return {};
				}
			;

			auto fDeleteVersionFromDisk = [&](CStr _App, CVersionManager::CVersionIDAndPlatform _VersionID) -> TCFuture<void>
				{
					auto BlockingActorCheckout = fg_BlockingActor();

					co_await
						(
							g_Dispatch(BlockingActorCheckout) / [=, VersionManagerDirectory = VersionManagerDirectory]()
							{
								CStr ApplicationDirectory = VersionManagerDirectory / "Applications" / _App;
								CStr VersionPath = ApplicationDirectory / _VersionID.f_EncodeFileName();
								CStr VersionInfoPath = VersionPath + ".json";

								if (CFile::fs_FileExists(VersionPath))
									CFile::fs_DeleteDirectoryRecursive(VersionPath);
								if (CFile::fs_FileExists(VersionInfoPath))
									CFile::fs_DeleteFile(VersionInfoPath);
							}
						)
					;

					co_return {};
				}
			;

			auto fModifyVersionOnDisk = [&](CStr _App, CVersionManager::CVersionIDAndPlatform _VersionID, CVersionManager::CVersionInformation _Info) -> TCFuture<void>
				{
					auto BlockingActorCheckout = fg_BlockingActor();

					co_await
						(
							g_Dispatch(BlockingActorCheckout) / [=, VersionManagerDirectory = VersionManagerDirectory]()
							{
								CStr ApplicationDirectory = VersionManagerDirectory / "Applications" / _App;
								CStr VersionPath = ApplicationDirectory / _VersionID.f_EncodeFileName();
								CStr VersionInfoPath = VersionPath + ".json";

								CEJsonSorted VersionInfo = EJsonType_Object;
								VersionInfo["Time"] = _Info.m_Time;
								VersionInfo["Configuration"] = _Info.m_Configuration;
								if (!_Info.m_Tags.f_IsEmpty())
								{
									CEJsonSorted Tags = EJsonType_Array;
									for (auto &Tag : _Info.m_Tags)
										Tags.f_Insert(_Info.m_Tags.fs_GetKey(Tag));
									VersionInfo["Tags"] = Tags;
								}
								CFile::fs_WriteStringToFile(VersionInfoPath, VersionInfo.f_ToString(nullptr));
							}
						)
					;

					co_return {};
				}
			;

			{
				DMibTestPath("RefreshDetectsExistingVersions");
				// TestApp was already uploaded by f_Setup
				auto Result = co_await fg_CallSafe(fRefreshVersions);
				// Should not detect any new versions since TestApp was uploaded through the API
				DMibExpect(Result.m_nAdded, ==, 0);
				DMibExpect(Result.m_nRemoved, ==, 0);
			}

			CVersionManager::CVersionIDAndPlatform NewVersionID;
			NewVersionID.m_VersionID.m_Branch = State.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
			NewVersionID.m_VersionID.m_Major = 2;
			NewVersionID.m_VersionID.m_Minor = 0;
			NewVersionID.m_VersionID.m_Revision = 0;
			NewVersionID.m_Platform = State.m_PackageInfo.m_VersionID.m_Platform;

			CVersionManager::CVersionInformation NewVersionInfo;
			NewVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
			NewVersionInfo.m_Configuration = "Release";
			NewVersionInfo.m_Tags["TestTag"];

			{
				DMibTestPath("RefreshDetectsNewVersionOnDisk");
				co_await fg_CallSafe(fCreateVersionOnDisk, "TestApp", NewVersionID, NewVersionInfo);
				auto Result = co_await fg_CallSafe(fRefreshVersions);
				DMibExpect(Result.m_nAdded, ==, 1);

				// Verify version is now visible via API
				auto Versions = co_await fg_CallSafe(fListVersions, "TestApp");
				DMibExpect(Versions.m_Versions["TestApp"].f_Exists(NewVersionID), ==, true);
			}

			{
				DMibTestPath("RefreshDetectsRemovedVersion");
				co_await fg_CallSafe(fDeleteVersionFromDisk, "TestApp", NewVersionID);
				auto Result = co_await fg_CallSafe(fRefreshVersions);
				DMibExpect(Result.m_nRemoved, ==, 1);

				// Verify version is no longer visible
				auto Versions = co_await fg_CallSafe(fListVersions, "TestApp");
				DMibExpect(Versions.m_Versions["TestApp"].f_Exists(NewVersionID), ==, false);
			}

			{
				DMibTestPath("RefreshDetectsUpdatedVersion");
				// First create the version again
				co_await fg_CallSafe(fCreateVersionOnDisk, "TestApp", NewVersionID, NewVersionInfo);
				auto CreateResult = co_await fg_CallSafe(fRefreshVersions);
				DMibExpect(CreateResult.m_nAdded, ==, 1);

				// Now modify it
				CVersionManager::CVersionInformation ModifiedInfo = NewVersionInfo;
				ModifiedInfo.m_Configuration = "Debug";
				ModifiedInfo.m_Time = NTime::CTime::fs_NowUTC() + NTime::CTimeSpanConvert::fs_CreateSecondSpan(1);
				co_await fg_CallSafe(fModifyVersionOnDisk, "TestApp", NewVersionID, ModifiedInfo);

				auto UpdateResult = co_await fg_CallSafe(fRefreshVersions);
				DMibExpect(UpdateResult.m_nUpdated, ==, 1);

				// Verify the update was applied
				auto Versions = co_await fg_CallSafe(fListVersions, "TestApp");
				auto *pVersion = Versions.m_Versions["TestApp"].f_FindEqual(NewVersionID);
				DMibExpectTrue(pVersion != nullptr);
				if (pVersion)
					DMibExpect(pVersion->m_Configuration, ==, "Debug");
			}

			co_return {};
		};

		DMibTestSuite("RaceConditions") -> TCFuture<void>
		{
			CAppManagerTestHelper::EOption Options
				= CAppManagerTestHelper::EOption_EnableVersionManager
				| CAppManagerTestHelper::EOption_DisablePatchMonitoring
				| CAppManagerTestHelper::EOption_DisableDiskMonitoring
				| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
				| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
			;

			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("VersionManagerRefreshTests_Race", Options, g_Timeout);
			auto &State = *AppManagerTestHelper.m_pState;

			auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

			co_await AppManagerTestHelper.f_Setup(1);

			CStr VersionManagerPath = State.m_VersionManagerDirectory / "VersionManager";
			CStr VersionManagerDirectory = State.m_VersionManagerDirectory;

			auto fRefreshVersions = [&]() -> TCFuture<CRefreshResult>
				{
					CStr Output = co_await AppManagerTestHelper.f_LaunchTool(VersionManagerPath, {"--refresh-versions"}, VersionManagerDirectory);
					co_return CRefreshResult::fs_Parse(Output);
				}
			;

			auto fListVersions = [&](CStr _Application) -> TCFuture<CVersionManager::CListVersions::CResult>
				{
					CVersionManager::CListVersions Params;
					Params.m_ForApplication = _Application;
					co_return co_await State.m_VersionManager.f_CallActor(&CVersionManager::f_ListVersions)(Params)
						.f_Timeout(g_Timeout, "Timed out waiting for ListVersions")
					;
				}
			;

			auto fUploadVersion = [&](CVersionManager::CVersionIDAndPlatform _VersionID, CVersionManager::CVersionInformation _VersionInfo) -> TCFuture<CVersionManagerHelper::CUploadResult>
				{
					co_return co_await State.m_VersionManagerHelper.f_Upload
						(
							State.m_VersionManager
							, "TestApp"
							, _VersionID
							, _VersionInfo
							, State.m_TestAppArchive
						)
					;
				}
			;

			{
				DMibTestPath("UploadAndRefreshConcurrent");
				// Start upload and refresh concurrently
				// Due to synchronization, refresh should wait for upload to complete
				CVersionManager::CVersionIDAndPlatform UploadVersionID;
				UploadVersionID.m_VersionID.m_Branch = State.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
				UploadVersionID.m_VersionID.m_Major = 3;
				UploadVersionID.m_VersionID.m_Minor = 0;
				UploadVersionID.m_VersionID.m_Revision = 0;
				UploadVersionID.m_Platform = State.m_PackageInfo.m_VersionID.m_Platform;

				CVersionManager::CVersionInformation UploadVersionInfo;
				UploadVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
				UploadVersionInfo.m_Configuration = "Release";
				UploadVersionInfo.m_Tags["TestTag"];

				auto UploadFuture = fg_CallSafe(fUploadVersion, UploadVersionID, UploadVersionInfo);
				auto RefreshFuture = fg_CallSafe(fRefreshVersions);

				auto UploadResult = co_await fg_Move(UploadFuture).f_Wrap();
				auto RefreshResult = co_await fg_Move(RefreshFuture).f_Wrap();

				DMibExpectNoException(UploadResult.f_Access());
				DMibExpectNoException(RefreshResult.f_Access());

				// Verify version is present
				auto Versions = co_await fg_CallSafe(fListVersions, "TestApp");
				DMibExpect(Versions.m_Versions["TestApp"].f_Exists(UploadVersionID), ==, true);
			}

			{
				DMibTestPath("MultipleUploadsWithRefresh");
				// Start multiple uploads, then start refresh
				// Refresh should wait for all uploads to complete

				TCFutureVector<CVersionManagerHelper::CUploadResult> UploadFutures;

				for (mint i = 0; i < 3; ++i)
				{
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = State.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 4;
					VersionID.m_VersionID.m_Minor = (uint32)i;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = State.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["TestTag"];

					fg_CallSafe(fUploadVersion, VersionID, VersionInfo) > UploadFutures;
				}

				auto RefreshFuture = fg_CallSafe(fRefreshVersions);

				auto UploadResults = co_await fg_AllDone(UploadFutures).f_Wrap();
				auto RefreshResult = co_await fg_Move(RefreshFuture).f_Wrap();

				DMibAssertNoException(UploadResults.f_Access());
				DMibAssertNoException(RefreshResult.f_Access());

				DMibExpect(RefreshResult->m_nAdded, ==, 0);

				// Verify all versions are present
				auto Versions = co_await fg_CallSafe(fListVersions, "TestApp");
				for (mint i = 0; i < 3; ++i)
				{
					DMibTestPath("{}"_f << i);
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = State.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 4;
					VersionID.m_VersionID.m_Minor = (uint32)i;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = State.m_PackageInfo.m_VersionID.m_Platform;
					DMibExpectTrue(Versions.m_Versions["TestApp"].f_Exists(VersionID));
				}
			}

			{
				DMibTestPath("ConcurrentRefreshRequests");
				// Two concurrent refresh requests should serialize and both complete
				auto Refresh1 = fg_CallSafe(fRefreshVersions);
				auto Refresh2 = fg_CallSafe(fRefreshVersions);
				auto Refresh3 = fg_CallSafe(fRefreshVersions);
				auto Refresh4 = fg_CallSafe(fRefreshVersions);
				auto Refresh5 = fg_CallSafe(fRefreshVersions);
				auto Refresh6 = fg_CallSafe(fRefreshVersions);
				auto Refresh7 = fg_CallSafe(fRefreshVersions);

				auto Result1 = co_await fg_Move(Refresh1).f_Wrap();
				auto Result2 = co_await fg_Move(Refresh2).f_Wrap();
				auto Result3 = co_await fg_Move(Refresh3).f_Wrap();
				auto Result4 = co_await fg_Move(Refresh4).f_Wrap();
				auto Result5 = co_await fg_Move(Refresh5).f_Wrap();
				auto Result6 = co_await fg_Move(Refresh6).f_Wrap();
				auto Result7 = co_await fg_Move(Refresh7).f_Wrap();

				DMibExpectNoException(Result1.f_Access());
				DMibExpectNoException(Result2.f_Access());
				DMibExpectNoException(Result3.f_Access());
				DMibExpectNoException(Result4.f_Access());
				DMibExpectNoException(Result5.f_Access());
				DMibExpectNoException(Result6.f_Access());
				DMibExpectNoException(Result7.f_Access());
			}

			co_return {};
		};

		DMibTestSuite("Ordering") -> TCFuture<void>
		{
			CAppManagerTestHelper::EOption Options
				= CAppManagerTestHelper::EOption_EnableVersionManager
				| CAppManagerTestHelper::EOption_DisablePatchMonitoring
				| CAppManagerTestHelper::EOption_DisableDiskMonitoring
				| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
				| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
			;

			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("VersionManagerRefreshTests_Order", Options, g_Timeout);
			auto &State = *AppManagerTestHelper.m_pState;

			auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

			co_await AppManagerTestHelper.f_Setup(1);

			CStr VersionManagerPath = State.m_VersionManagerDirectory / "VersionManager";
			CStr VersionManagerDirectory = State.m_VersionManagerDirectory;

			auto fRefreshVersions = [&]() -> TCFuture<CRefreshResult>
				{
					CStr Output = co_await AppManagerTestHelper.f_LaunchTool(VersionManagerPath, {"--refresh-versions"}, VersionManagerDirectory);
					co_return CRefreshResult::fs_Parse(Output);
				}
			;

			auto fUploadVersion = [&](CVersionManager::CVersionIDAndPlatform _VersionID, CVersionManager::CVersionInformation _VersionInfo) -> TCFuture<CVersionManagerHelper::CUploadResult>
				{
					co_return co_await State.m_VersionManagerHelper.f_Upload
						(
							State.m_VersionManager
							, "TestApp"
							, _VersionID
							, _VersionInfo
							, State.m_TestAppArchive
						)
					;
				}
			;

			{
				DMibTestPath("UploadThenImmediateRefresh");
				// Complete an upload, then immediately refresh
				// Refresh should not add the version since it was already added via upload
				CVersionManager::CVersionIDAndPlatform VersionID;
				VersionID.m_VersionID.m_Branch = State.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
				VersionID.m_VersionID.m_Major = 5;
				VersionID.m_VersionID.m_Minor = 0;
				VersionID.m_VersionID.m_Revision = 0;
				VersionID.m_Platform = State.m_PackageInfo.m_VersionID.m_Platform;

				CVersionManager::CVersionInformation VersionInfo;
				VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
				VersionInfo.m_Configuration = "Release";
				VersionInfo.m_Tags["TestTag"];

				co_await fg_CallSafe(fUploadVersion, VersionID, VersionInfo);

				auto RefreshResult = co_await fg_CallSafe(fRefreshVersions);
				// The uploaded version should already be known, so no additions
				DMibExpect(RefreshResult.m_nAdded, ==, 0);
			}

			{
				DMibTestPath("RefreshThenImmediateUpload");
				// Do a refresh, then immediately upload
				// Upload should succeed

				co_await fg_CallSafe(fRefreshVersions);

				CVersionManager::CVersionIDAndPlatform VersionID;
				VersionID.m_VersionID.m_Branch = State.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
				VersionID.m_VersionID.m_Major = 6;
				VersionID.m_VersionID.m_Minor = 0;
				VersionID.m_VersionID.m_Revision = 0;
				VersionID.m_Platform = State.m_PackageInfo.m_VersionID.m_Platform;

				CVersionManager::CVersionInformation VersionInfo;
				VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
				VersionInfo.m_Configuration = "Release";
				VersionInfo.m_Tags["TestTag"];

				auto UploadResult = co_await fg_CallSafe(fUploadVersion, VersionID, VersionInfo).f_Wrap();
				DMibExpectNoException(UploadResult.f_Access());
			}

			co_return {};
		};

		DMibTestSuite("EdgeCases") -> TCFuture<void>
		{
			CAppManagerTestHelper::EOption Options
				= CAppManagerTestHelper::EOption_EnableVersionManager
				| CAppManagerTestHelper::EOption_DisablePatchMonitoring
				| CAppManagerTestHelper::EOption_DisableDiskMonitoring
				| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
				| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
			;

			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("VersionManagerRefreshTests_Edge", Options, g_Timeout);
			auto &State = *AppManagerTestHelper.m_pState;

			auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

			co_await AppManagerTestHelper.f_Setup(1);

			CStr VersionManagerPath = State.m_VersionManagerDirectory / "VersionManager";
			CStr VersionManagerDirectory = State.m_VersionManagerDirectory;

			auto fRefreshVersions = [&]() -> TCFuture<CRefreshResult>
				{
					CStr Output = co_await AppManagerTestHelper.f_LaunchTool(VersionManagerPath, {"--refresh-versions"}, VersionManagerDirectory);
					co_return CRefreshResult::fs_Parse(Output);
				}
			;

			auto fListVersions = [&](CStr _Application) -> TCFuture<CVersionManager::CListVersions::CResult>
				{
					CVersionManager::CListVersions Params;
					Params.m_ForApplication = _Application;
					co_return co_await State.m_VersionManager.f_CallActor(&CVersionManager::f_ListVersions)(Params)
						.f_Timeout(g_Timeout, "Timed out waiting for ListVersions")
					;
				}
			;

			auto fCreateVersionDirOnly = [&](CStr _App, CVersionManager::CVersionIDAndPlatform _VersionID) -> TCFuture<void>
				{
					auto BlockingActorCheckout = fg_BlockingActor();

					co_await
						(
							g_Dispatch(BlockingActorCheckout) / [=, VersionManagerDirectory = VersionManagerDirectory]()
							{
								CStr ApplicationDirectory = VersionManagerDirectory / "Applications" / _App;
								CFile::fs_CreateDirectory(ApplicationDirectory);

								CStr VersionPath = ApplicationDirectory / _VersionID.f_EncodeFileName();
								CFile::fs_CreateDirectory(VersionPath);

								// Create a dummy file so the version has content
								CFile::fs_WriteStringToFile(VersionPath / "dummy.txt", "test content");
								// No .json file - testing missing metadata
							}
						)
					;

					co_return {};
				}
			;

			auto fCreateVersionWithCorruptJson = [&](CStr _App, CVersionManager::CVersionIDAndPlatform _VersionID) -> TCFuture<void>
				{
					auto BlockingActorCheckout = fg_BlockingActor();

					co_await
						(
							g_Dispatch(BlockingActorCheckout) / [=, VersionManagerDirectory = VersionManagerDirectory]()
							{
								CStr ApplicationDirectory = VersionManagerDirectory / "Applications" / _App;
								CFile::fs_CreateDirectory(ApplicationDirectory);

								CStr VersionPath = ApplicationDirectory / _VersionID.f_EncodeFileName();
								CFile::fs_CreateDirectory(VersionPath);

								// Create a dummy file
								CFile::fs_WriteStringToFile(VersionPath / "dummy.txt", "test content");

								// Write corrupted JSON
								CStr VersionInfoPath = VersionPath + ".json";
								CFile::fs_WriteStringToFile(VersionInfoPath, "{ this is not valid json }}}");
							}
						)
					;

					co_return {};
				}
			;

			{
				DMibTestPath("RefreshWithMissingJsonFile");
				// Create version directory without .json file
				// Refresh should still detect the version (with default info)
				CVersionManager::CVersionIDAndPlatform VersionID;
				VersionID.m_VersionID.m_Branch = State.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
				VersionID.m_VersionID.m_Major = 7;
				VersionID.m_VersionID.m_Minor = 0;
				VersionID.m_VersionID.m_Revision = 0;
				VersionID.m_Platform = State.m_PackageInfo.m_VersionID.m_Platform;

				co_await fg_CallSafe(fCreateVersionDirOnly, "TestApp", VersionID);
				auto Result = co_await fg_CallSafe(fRefreshVersions);
				DMibExpect(Result.m_nAdded, ==, 1);

				// Verify version is visible
				auto Versions = co_await fg_CallSafe(fListVersions, "TestApp");
				DMibExpectTrue(Versions.m_Versions["TestApp"].f_Exists(VersionID));
			}

			{
				DMibTestPath("RefreshWithCorruptedJsonFile");
				// Create version with corrupted .json file
				// Refresh should handle gracefully (log error, skip version)
				CVersionManager::CVersionIDAndPlatform VersionID;
				VersionID.m_VersionID.m_Branch = State.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
				VersionID.m_VersionID.m_Major = 8;
				VersionID.m_VersionID.m_Minor = 0;
				VersionID.m_VersionID.m_Revision = 0;
				VersionID.m_Platform = State.m_PackageInfo.m_VersionID.m_Platform;

				co_await fg_CallSafe(fCreateVersionWithCorruptJson, "TestApp", VersionID);

				// Refresh should not crash - it should log an error and continue
				auto Result = co_await fg_CallSafe(fRefreshVersions).f_Wrap();
				DMibExpectNoException(Result.f_Access());
				// Note: The version may or may not be added depending on error handling
				// The important thing is that the refresh completes without crashing
			}

			co_return {};
		};

		DMibTestSuite("TagDatabasePersistence") -> TCFuture<void>
		{
			CAppManagerTestHelper::EOption Options
				= CAppManagerTestHelper::EOption_EnableVersionManager
				| CAppManagerTestHelper::EOption_DisablePatchMonitoring
				| CAppManagerTestHelper::EOption_DisableDiskMonitoring
				| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
				| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
			;

			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("VersionManagerRefreshTests_TagDb", Options, g_Timeout);
			auto &State = *AppManagerTestHelper.m_pState;

			auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

			co_await AppManagerTestHelper.f_Setup(1);

			CStr VersionManagerPath = State.m_VersionManagerDirectory / "VersionManager";
			CStr VersionManagerDirectory = State.m_VersionManagerDirectory;

			auto fRefreshVersions = [&]() -> TCFuture<CRefreshResult>
				{
					CStr Output = co_await AppManagerTestHelper.f_LaunchTool(VersionManagerPath, {"--refresh-versions"}, VersionManagerDirectory);
					co_return CRefreshResult::fs_Parse(Output);
				}
			;

			auto fChangeTags = [&](CStr _App, CVersionManager::CVersionIDAndPlatform _VersionID, TCSet<CStr> _AddTags, TCSet<CStr> _RemoveTags) -> TCFuture<void>
				{
					CVersionManager::CChangeTags Params;
					Params.m_Application = _App;
					Params.m_VersionID = _VersionID.m_VersionID;
					Params.m_Platform = _VersionID.m_Platform;
					Params.m_AddTags = _AddTags;
					Params.m_RemoveTags = _RemoveTags;
					co_await State.m_VersionManager.f_CallActor(&CVersionManager::f_ChangeTags)(fg_Move(Params))
						.f_Timeout(g_Timeout, "Timed out waiting for ChangeTags")
					;
					co_return {};
				}
			;

			auto fListVersions = [&](CStr _Application) -> TCFuture<CVersionManager::CListVersions::CResult>
				{
					CVersionManager::CListVersions Params;
					Params.m_ForApplication = _Application;
					co_return co_await State.m_VersionManager.f_CallActor(&CVersionManager::f_ListVersions)(Params)
						.f_Timeout(g_Timeout, "Timed out waiting for ListVersions")
					;
				}
			;

			{
				DMibTestPath("TagChangesSavedToDatabase");
				// TestApp was uploaded by f_Setup with TestTag
				// Add a new tag and verify it persists after refresh

				TCSet<CStr> AddTags;
				AddTags["NewTestTag"];

				co_await fg_CallSafe(fChangeTags, "TestApp", State.m_PackageInfo.m_VersionID, AddTags, TCSet<CStr>{});

				// Verify the tag was added
				auto VersionsBefore = co_await fg_CallSafe(fListVersions, "TestApp");
				auto *pVersionBefore = VersionsBefore.m_Versions["TestApp"].f_FindEqual(State.m_PackageInfo.m_VersionID);
				DMibExpectTrue(pVersionBefore != nullptr);
				if (pVersionBefore)
					DMibExpectTrue(pVersionBefore->m_Tags.f_FindEqual("NewTestTag") != nullptr);

				// Refresh should detect no changes because the tag was saved to both disk and database
				auto RefreshResult = co_await fg_CallSafe(fRefreshVersions);
				DMibExpect(RefreshResult.m_nUpdated, ==, 0);

				// Verify tag is still present
				auto VersionsAfter = co_await fg_CallSafe(fListVersions, "TestApp");
				auto *pVersionAfter = VersionsAfter.m_Versions["TestApp"].f_FindEqual(State.m_PackageInfo.m_VersionID);
				DMibExpectTrue(pVersionAfter != nullptr);
				if (pVersionAfter)
					DMibExpectTrue(pVersionAfter->m_Tags.f_FindEqual("NewTestTag") != nullptr);
			}

			co_return {};
		};

		DMibTestSuite("RefreshNotifications") -> TCFuture<void>
		{
			CAppManagerTestHelper::EOption Options
				= CAppManagerTestHelper::EOption_EnableVersionManager
				| CAppManagerTestHelper::EOption_DisablePatchMonitoring
				| CAppManagerTestHelper::EOption_DisableDiskMonitoring
				| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
				| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
			;

			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

			CAppManagerTestHelper AppManagerTestHelper("VersionManagerRefreshTests_Notify", Options, g_Timeout);
			auto &State = *AppManagerTestHelper.m_pState;

			auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

			co_await AppManagerTestHelper.f_Setup(1);

			CStr VersionManagerPath = State.m_VersionManagerDirectory / "VersionManager";
			CStr VersionManagerDirectory = State.m_VersionManagerDirectory;

			auto fRefreshVersions = [&]() -> TCFuture<CRefreshResult>
				{
					CStr Output = co_await AppManagerTestHelper.f_LaunchTool(VersionManagerPath, {"--refresh-versions"}, VersionManagerDirectory);
					co_return CRefreshResult::fs_Parse(Output);
				}
			;

			auto fCreateVersionOnDisk = [&](CStr _App, CVersionManager::CVersionIDAndPlatform _VersionID, CVersionManager::CVersionInformation _Info) -> TCFuture<void>
				{
					auto BlockingActorCheckout = fg_BlockingActor();

					co_await
						(
							g_Dispatch(BlockingActorCheckout) / [=, VersionManagerDirectory = VersionManagerDirectory]()
							{
								CStr ApplicationDirectory = VersionManagerDirectory / "Applications" / _App;
								CFile::fs_CreateDirectory(ApplicationDirectory);

								CStr VersionPath = ApplicationDirectory / _VersionID.f_EncodeFileName();
								CFile::fs_CreateDirectory(VersionPath);

								// Create a dummy file so the version has content
								CFile::fs_WriteStringToFile(VersionPath / "dummy.txt", "test content");

								// Write version info JSON
								CStr VersionInfoPath = VersionPath + ".json";
								CEJsonSorted VersionInfo = EJsonType_Object;
								VersionInfo["Time"] = _Info.m_Time;
								VersionInfo["Configuration"] = _Info.m_Configuration;
								if (!_Info.m_Tags.f_IsEmpty())
								{
									CEJsonSorted Tags = EJsonType_Array;
									for (auto &Tag : _Info.m_Tags)
										Tags.f_Insert(_Info.m_Tags.fs_GetKey(Tag));
									VersionInfo["Tags"] = Tags;
								}
								CFile::fs_WriteStringToFile(VersionInfoPath, VersionInfo.f_ToString(nullptr));
							}
						)
					;

					co_return {};
				}
			;

			{
				DMibTestPath("RefreshSendsNotificationsForNewVersions");

				// Set up subscription tracking
				TCVector<CVersionManager::CNewVersionNotification> ReceivedNotifications;
				TCPromise<void> NotificationReceived;
				bool bReceivedNotification = false;

				CVersionManager::CSubscribeToUpdates SubscribeParams;
				SubscribeParams.m_Application = "TestApp";
				SubscribeParams.m_nInitial = 0; // Don't send existing versions

				SubscribeParams.m_fOnNewVersions = g_ActorFunctor / [&](CVersionManager::CNewVersionNotifications _Notifications) -> TCFuture<CVersionManager::CNewVersionNotifications::CResult>
					{
						for (auto &Notification : _Notifications.m_NewVersions)
							ReceivedNotifications.f_InsertLast(Notification);

						if (!bReceivedNotification && !ReceivedNotifications.f_IsEmpty())
						{
							bReceivedNotification = true;
							NotificationReceived.f_SetResult();
						}

						co_return {};
					}
				;

				auto SubscribeResult = co_await State.m_VersionManager.f_CallActor(&CVersionManager::f_SubscribeToUpdates)(fg_Move(SubscribeParams))
					.f_Timeout(g_Timeout, "Timed out waiting for SubscribeToUpdates")
				;

				// Create a new version on disk
				CVersionManager::CVersionIDAndPlatform NewVersionID;
				NewVersionID.m_VersionID.m_Branch = State.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
				NewVersionID.m_VersionID.m_Major = 9;
				NewVersionID.m_VersionID.m_Minor = 0;
				NewVersionID.m_VersionID.m_Revision = 0;
				NewVersionID.m_Platform = State.m_PackageInfo.m_VersionID.m_Platform;

				CVersionManager::CVersionInformation NewVersionInfo;
				NewVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
				NewVersionInfo.m_Configuration = "Release";
				NewVersionInfo.m_Tags["TestTag"];

				co_await fg_CallSafe(fCreateVersionOnDisk, "TestApp", NewVersionID, NewVersionInfo);

				// Refresh should detect the new version and send a notification
				auto RefreshResult = co_await fg_CallSafe(fRefreshVersions);
				DMibExpect(RefreshResult.m_nAdded, ==, 1);

				// Wait for notification (with timeout)
				auto NotificationResult = co_await NotificationReceived.f_Future().f_Timeout(10.0, "Timed out waiting for notification").f_Wrap();
				DMibExpectNoException(NotificationResult.f_Access());

				// Verify we received the correct notification
				DMibExpect(ReceivedNotifications.f_GetLen(), >=, 1);
				if (!ReceivedNotifications.f_IsEmpty())
				{
					bool bFoundVersion = false;
					for (auto &Notification : ReceivedNotifications)
					{
						if (Notification.m_Application == "TestApp" && Notification.m_VersionIDAndPlatform == NewVersionID)
						{
							bFoundVersion = true;
							break;
						}
					}
					DMibExpectTrue(bFoundVersion);
				}
			}

			co_return {};
		};
	}
};

DMibTestRegister(CVersionManager_Refresh_Tests, Malterlib::Cloud);
