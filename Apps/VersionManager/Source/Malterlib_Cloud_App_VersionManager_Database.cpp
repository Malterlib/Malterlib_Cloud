// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Encoding/ToJson>
#include <Mib/Database/DatabaseActor>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"
#include "Malterlib_Cloud_App_VersionManager_Database.h"

namespace NMib::NCloud::NVersionManagerDatabase
{
	constexpr CStr CVersionDatabaseKey::mc_Prefix = gc_Str<"Version">;

	CEJsonSorted CVersionDatabaseKey::f_ToJson() const
	{
		CEJsonSorted Return;
		Return["Prefix"] = fg_ToJson(m_Prefix);
		Return["Application"] = fg_ToJson(m_Application);
		Return["VersionIDAndPlatform"] = m_VersionIDAndPlatform.f_ToJson();
		return Return;
	}

	CEJsonSorted CVersionDatabaseValue::f_ToJson() const
	{
		CEJsonSorted Return;
		Return["VersionInfo"] = m_VersionInfo.f_ToJson();
		return Return;
	}
}

namespace NMib::NCloud::NVersionManager
{
	using namespace NVersionManagerDatabase;

	constinit uint64 g_MaxDatabaseSize = constant_uint64(1) * 1024 * 1024 * 1024; // 1GB default

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SetupDatabase()
	{
		mp_DatabaseActor = fg_Construct();
		auto MaxDatabaseSize = mp_AppState.m_ConfigDatabase.m_Data.f_GetMemberValue("MaxDatabaseSize", g_MaxDatabaseSize).f_Integer();

		co_await
			(
				mp_DatabaseActor
				(
					&CDatabaseActor::f_OpenDatabase
					, mp_AppState.m_RootDirectory / "VersionManagerDatabase"
					, MaxDatabaseSize
				)
				% "Failed to open database"
			)
		;

		auto Stats = co_await mp_DatabaseActor(&CDatabaseActor::f_GetAggregateStatistics);
		auto TotalSizeUsed = Stats.f_GetTotalUsedSize();
		DLogWithCategory
			(
				Malterlib/Cloud/VersionManager
				, Info
				, "Database uses {fe2}% of allotted space ({ns } / {ns } bytes). {ns } records."
				, fp64(TotalSizeUsed) / fp64(MaxDatabaseSize) * 100.0
				, TotalSizeUsed
				, MaxDatabaseSize
				, Stats.m_nDataItems
			)
		;

		co_return {};
	}

	TCFuture<bool> CVersionManagerDaemonActor::CServer::fp_LoadVersionsFromDatabase()
	{
		auto ReadTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead);

		struct CLoadResult
		{
			TCMap<CStr, TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManager::CVersionInformation>> m_Versions;
			TCSet<CStr> m_KnownTags;
		};

		auto Result = co_await fg_Move(ReadTransaction).f_BlockingDispatch
			(
				[](CDatabaseActor::CTransactionRead &&_ReadTransaction) -> CLoadResult
				{
					CLoadResult Result;

					for (auto ReadCursor = _ReadTransaction.m_Transaction.f_ReadCursor(CVersionDatabaseKey::mc_Prefix); ReadCursor; ++ReadCursor)
					{
						auto Key = ReadCursor.f_Key<CVersionDatabaseKey>();
						auto Value = ReadCursor.f_Value<CVersionDatabaseValue>();

						Result.m_Versions[Key.m_Application][Key.m_VersionIDAndPlatform] = Value.m_VersionInfo;
						Result.m_KnownTags += Value.m_VersionInfo.m_Tags;
					}

					return Result;
				}
			)
		;

		if (Result.m_Versions.f_IsEmpty())
			co_return false;

		// Populate mp_Applications from loaded data
		umint nVersions = 0;
		for (auto &ApplicationVersionsEntry : Result.m_Versions.f_Entries())
		{
			auto &Application = mp_Applications[ApplicationVersionsEntry.f_Key()];
			for (auto &VersionEntry : ApplicationVersionsEntry.f_Value().f_Entries())
			{
				auto &OutVersion = Application.m_Versions[VersionEntry.f_Key()];
				OutVersion.m_VersionInfo = VersionEntry.f_Value();
				Application.m_VersionsByTime.f_Insert(OutVersion);
				++nVersions;
			}
		}

		mp_KnownTags = fg_Move(Result.m_KnownTags);

		DLogWithCategory
			(
				Malterlib/Cloud/VersionManager
				, Info
				, "Loaded {ns } applications with {ns } versions from database"
				, mp_Applications.f_GetLen()
				, nVersions
			)
		;

		co_return true;
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_SaveVersionToDatabase(CStr _Application, CVersionManager::CVersionIDAndPlatform _VersionID, CVersionManager::CVersionInformation _VersionInfo)
	{
		TCSharedPointer<CCanDestroyTracker> pCanDestroy = mp_pCanDestroyTracker;
		if (!pCanDestroy)
			co_return DMibErrorInstance("Shutting down");

		CVersionDatabaseKey Key;
		Key.m_Application = fg_Move(_Application);
		Key.m_VersionIDAndPlatform = fg_Move(_VersionID);

		CVersionDatabaseValue Value;
		Value.m_VersionInfo = fg_Move(_VersionInfo);

		auto WriteTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionWrite);
		auto Checkout = WriteTransaction.f_Checkout().f_Actor();

		WriteTransaction = co_await
			(
				g_Dispatch(Checkout) / [WriteTransaction = fg_Move(WriteTransaction), Key = fg_Move(Key), Value = fg_Move(Value)]() mutable -> CDatabaseActor::CTransactionWrite
				{
					auto Cursor = WriteTransaction.m_Transaction.f_WriteCursor();
					Cursor.f_Upsert(Key, Value);
					return fg_Move(WriteTransaction);
				}
			)
		;

		co_await mp_DatabaseActor(&CDatabaseActor::f_CommitWriteTransaction, fg_Move(WriteTransaction));

		co_return {};
	}

	TCFuture<void> CVersionManagerDaemonActor::CServer::fp_RemoveVersionFromDatabase(CStr _Application, CVersionManager::CVersionIDAndPlatform _VersionID)
	{
		TCSharedPointer<CCanDestroyTracker> pCanDestroy = mp_pCanDestroyTracker;
		if (!pCanDestroy)
			co_return DMibErrorInstance("Shutting down");

		CVersionDatabaseKey Key;
		Key.m_Application = fg_Move(_Application);
		Key.m_VersionIDAndPlatform = fg_Move(_VersionID);

		auto WriteTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionWrite);
		auto Checkout = WriteTransaction.f_Checkout().f_Actor();

		WriteTransaction = co_await
			(
				g_Dispatch(Checkout) / [WriteTransaction = fg_Move(WriteTransaction), Key = fg_Move(Key)]() mutable -> CDatabaseActor::CTransactionWrite
				{
					auto Cursor = WriteTransaction.m_Transaction.f_WriteCursor();
					if (Cursor.f_FindEqual(Key))
						Cursor.f_Delete();
					return fg_Move(WriteTransaction);
				}
			)
		;

		co_await mp_DatabaseActor(&CDatabaseActor::f_CommitWriteTransaction, fg_Move(WriteTransaction));

		co_return {};
	}

	void CVersionManagerDaemonActor::CServer::fp_NotifyUploadsEmpty()
	{
		if (mp_nInProgressUploads == 0 && !mp_UploadsEmptyWaiters.f_IsEmpty())
		{
			for (auto &Waiter : mp_UploadsEmptyWaiters)
				Waiter.f_SetResult();
			mp_UploadsEmptyWaiters.f_Clear();
		}
	}

	TCFuture<CVersionManagerDaemonActor::CServer::CRefreshResult> CVersionManagerDaemonActor::CServer::f_RefreshDatabaseFromDisk()
	{
		CRefreshResult RefreshResult;

		// Step 1: Take sequencer to block new uploads
		auto SequenceSubscription = co_await mp_RefreshSequencer.f_Sequence();

		// Step 2: Set flag so new uploads will wait on sequencer
		mp_bRefreshInProgress = true;
		auto ClearFlag = g_OnScopeExit / [this]
			{
				mp_bRefreshInProgress = false;
			}
		;

		// Step 3: Wait for in-progress uploads to complete
		while (mp_nInProgressUploads > 0)
		{
			TCPromiseFuturePair<void> Waiter;
			mp_UploadsEmptyWaiters.f_InsertLast(fg_Move(Waiter.m_Promise));
			co_await fg_Move(Waiter.m_Future);
		}

		// Step 5: Define result struct for blocking dispatch
		struct CBlockingWorkResult
		{
			TCMap<CStr, TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManager::CVersionInformation>> m_DiskVersions;
			TCSet<CStr> m_KnownTags;
			umint m_nAdded = 0;
			umint m_nUpdated = 0;
			umint m_nRemoved = 0;
			TCVector<TCTuple<CStr, CVersionManager::CVersionIDAndPlatform>> m_AddedOrUpdated;
			CDatabaseActor::CTransactionWrite m_WriteTransaction;
		};

		auto WriteTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionWrite);
		auto Checkout = WriteTransaction.f_Checkout().f_Actor();

		// Use g_Dispatch with the write transaction's checkout to run blocking work
		// This returns to CServer actor when complete
		auto WorkResult = co_await
			(
				g_Dispatch(Checkout) / [WriteTransactionTemp = fg_Move(WriteTransaction), RootDirectory = mp_AppState.m_RootDirectory]() mutable -> CBlockingWorkResult
				{
					CBlockingWorkResult Result{.m_WriteTransaction = fg_Move(WriteTransactionTemp)};

					CStr ApplicationDirectory = RootDirectory / "Applications";
					CFile::CFindFilesOptions FindOptions(ApplicationDirectory / "*", false);
					FindOptions.m_AttribMask = EFileAttrib_Directory;
					auto FoundFiles = CFile::fs_FindFiles(FindOptions);
					TCSet<CStr> Applications;
					for (auto &File : FoundFiles)
					{
						CStr Application = File.m_Path.f_Extract(ApplicationDirectory.f_GetLen() + 1);
						if (CVersionManager::fs_IsValidApplicationName(Application))
							Applications[Application];
					}

					// === Disk scan ===
					for (auto &Application : Applications)
					{
						auto &Versions = Result.m_DiskVersions[Application];
						CStr ApplicationPath = fg_Format("{}/{}", ApplicationDirectory, Application);
						CFile::CFindFilesOptions FindOptions(ApplicationPath + "/*", false);
						FindOptions.m_AttribMask = EFileAttrib_Directory;
						auto FoundFiles = CFile::fs_FindFiles(FindOptions);
						for (auto &File : FoundFiles)
						{
							CStr Version = CVersionManager::CVersionID::fs_DecodeFileName(File.m_Path.f_Extract(ApplicationPath.f_GetLen() + 1));
							CVersionManager::CVersionIDAndPlatform VersionID;
							CStr Error;
							if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID.m_VersionID))
								continue;
							CStr VersionIDPath = fg_Format("{}/{}", ApplicationPath, VersionID.m_VersionID.f_EncodeFileName());
							CFile::CFindFilesOptions PlatformFindOptions(VersionIDPath + "/*", false);
							PlatformFindOptions.m_AttribMask = EFileAttrib_Directory;
							auto PlatformFiles = CFile::fs_FindFiles(PlatformFindOptions);
							for (auto &PlatformFile : PlatformFiles)
							{
								CStr Platform = PlatformFile.m_Path.f_Extract(VersionIDPath.f_GetLen() + 1);
								if (!CVersionManager::fs_IsValidPlatform(Platform))
									continue;
								VersionID.m_Platform = Platform;
								try
								{
									CStr VersionPath = fg_Format("{}/{}", ApplicationPath, VersionID.f_EncodeFileName());
									CStr VersionInfoPath = fg_Format("{}.json", VersionPath);
									CVersionManager::CVersionInformation OutVersion;
									if (CFile::fs_FileExists(VersionInfoPath))
									{
										CEJsonSorted ApplicationInfo = CEJsonSorted::fs_FromString(CFile::fs_ReadStringFromFile(VersionInfoPath), VersionInfoPath);
										if (auto pValue = ApplicationInfo.f_GetMember("Time", EEJsonType_Date))
											OutVersion.m_Time = pValue->f_Date();
										if (auto pValue = ApplicationInfo.f_GetMember("Configuration", EJsonType_String))
											OutVersion.m_Configuration = pValue->f_String();
										if (auto pValue = ApplicationInfo.f_GetMember("ExtraInfo", EJsonType_Object))
											OutVersion.m_ExtraInfo = *pValue;
										if (auto pValue = ApplicationInfo.f_GetMember("Tags", EJsonType_Array))
										{
											for (auto &Value : pValue->f_Array())
											{
												if (Value.f_IsString())
													OutVersion.m_Tags[Value.f_String()];
											}
										}
										if (auto pValue = ApplicationInfo.f_GetMember("RetrySequence", EJsonType_Integer))
											OutVersion.m_RetrySequence = pValue->f_Integer();
									}
									{
										auto Files = CFile::fs_FindFiles(VersionPath + "/*", EFileAttrib_File, true);
										OutVersion.m_nFiles = Files.f_GetLen();
										for (auto &SizeFile : Files)
											OutVersion.m_nBytes += CFile::fs_GetFileSize(SizeFile);
									}

									Versions[VersionID] = fg_Move(OutVersion);
								}
								catch (NException::CException const &Exception)
								{
									DLogWithCategory(Malterlib/Cloud/VersionManager, Error, "Internal error reading version info: {}", Exception.f_GetErrorStr());
								}
							}
						}
					}

					// === Read database from write transaction ===
					TCMap<CStr, TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManager::CVersionInformation>> DatabaseVersions;
					for (auto ReadCursor = Result.m_WriteTransaction.m_Transaction.f_ReadCursor(CVersionDatabaseKey::mc_Prefix); ReadCursor; ++ReadCursor)
					{
						auto Key = ReadCursor.f_Key<CVersionDatabaseKey>();
						auto Value = ReadCursor.f_Value<CVersionDatabaseValue>();
						DatabaseVersions[Key.m_Application][Key.m_VersionIDAndPlatform] = Value.m_VersionInfo;
					}

					// === Compare and build change lists (merged ToUpsert) ===
					TCVector<TCTuple<CStr, CVersionManager::CVersionIDAndPlatform, CVersionManager::CVersionInformation>> ToUpsert;
					TCVector<TCTuple<CStr, CVersionManager::CVersionIDAndPlatform>> ToRemove;

					// Find new/updated versions
					for (auto &AppVersionsEntry : Result.m_DiskVersions.f_Entries())
					{
						auto const &AppName = AppVersionsEntry.f_Key();
						auto *pDbAppVersions = DatabaseVersions.f_FindEqual(AppName);
						for (auto &VersionEntry : AppVersionsEntry.f_Value().f_Entries())
						{
							auto const &VersionID = VersionEntry.f_Key();
							auto const &Version = VersionEntry.f_Value();

							auto *pDbVersion = pDbAppVersions ? pDbAppVersions->f_FindEqual(VersionID) : nullptr;
							if (!pDbVersion)
							{
								ToUpsert.f_InsertLast(fg_TupleReferences(AppName, VersionID, Version));
								Result.m_AddedOrUpdated.f_InsertLast(fg_Tuple(AppName, VersionID));
								++Result.m_nAdded;
							}
							else if (*pDbVersion != Version)
							{
								ToUpsert.f_InsertLast(fg_TupleReferences(AppName, VersionID, Version));
								Result.m_AddedOrUpdated.f_InsertLast(fg_Tuple(AppName, VersionID));
								++Result.m_nUpdated;
							}
						}
					}

					// Find removed versions
					for (auto &AppVersionsEntry : DatabaseVersions.f_Entries())
					{
						auto const &AppName = AppVersionsEntry.f_Key();
						auto *pDiskAppVersions = Result.m_DiskVersions.f_FindEqual(AppName);
						for (auto &VersionID : AppVersionsEntry.f_Value().f_Keys())
						{
							if (!pDiskAppVersions || !pDiskAppVersions->f_FindEqual(VersionID))
							{
								ToRemove.f_InsertLast(fg_TupleReferences(AppName, VersionID));
								++Result.m_nRemoved;
							}
						}
					}

					// === Apply database changes ===
					if (!ToUpsert.f_IsEmpty() || !ToRemove.f_IsEmpty())
					{
						auto Cursor = Result.m_WriteTransaction.m_Transaction.f_WriteCursor();
						for (auto &[AppName, VersionID, VersionInfo] : ToUpsert)
						{
							CVersionDatabaseKey Key;
							Key.m_Application = AppName;
							Key.m_VersionIDAndPlatform = VersionID;
							CVersionDatabaseValue Value;
							Value.m_VersionInfo = VersionInfo;
							Cursor.f_Upsert(Key, Value);
						}

						for (auto &[AppName, VersionID] : ToRemove)
						{
							CVersionDatabaseKey Key;
							Key.m_Application = AppName;
							Key.m_VersionIDAndPlatform = VersionID;
							if (Cursor.f_FindEqual(Key))
								Cursor.f_Delete();
						}
					}

					// === Collect known tags ===
					for (auto &AppVersions : Result.m_DiskVersions)
						for (auto &Version : AppVersions)
							Result.m_KnownTags += Version.m_Tags;

					return Result;
				}
			)
		;

		// Commit the transaction - we're back on CServer actor now
		co_await mp_DatabaseActor(&CDatabaseActor::f_CommitWriteTransaction, fg_Move(WorkResult.m_WriteTransaction));

		// Step 6: Update in-memory state
		mp_Applications.f_Clear();
		mp_KnownTags.f_Clear();

		for (auto &ApplicationVersionsEntry : WorkResult.m_DiskVersions.f_Entries())
		{
			auto &Application = mp_Applications[ApplicationVersionsEntry.f_Key()];
			for (auto &VersionEntry : ApplicationVersionsEntry.f_Value().f_Entries())
			{
				auto &OutVersion = Application.m_Versions[VersionEntry.f_Key()];
				OutVersion.m_VersionInfo = VersionEntry.f_Value();
				Application.m_VersionsByTime.f_Insert(OutVersion);
			}
		}

		mp_KnownTags = fg_Move(WorkResult.m_KnownTags);

		TCFutureVector<void> NotificationResults;
		// Step 7: Send notifications for added/updated versions
		CStr OriginID = fg_RandomID();
		for (auto &[AppName, VersionID] : WorkResult.m_AddedOrUpdated)
		{
			auto *pApplication = mp_Applications.f_FindEqual(AppName);
			if (!pApplication)
				continue;

			auto *pVersion = pApplication->m_Versions.f_FindEqual(VersionID);
			if (!pVersion)
				continue;

			fp_NewVersion(AppName, pVersion->f_GetIdentifier(), pVersion->m_VersionInfo, OriginID) > NotificationResults;
		}

		co_await fg_AllDone(NotificationResults);

		RefreshResult.m_nAdded = WorkResult.m_nAdded;
		RefreshResult.m_nUpdated = WorkResult.m_nUpdated;
		RefreshResult.m_nRemoved = WorkResult.m_nRemoved;

		DLogWithCategory
			(
				Malterlib/Cloud/VersionManager
				, Info
				, "Refreshed version database: {} added, {} updated, {} removed"
				, RefreshResult.m_nAdded
				, RefreshResult.m_nUpdated
				, RefreshResult.m_nRemoved
			)
		;

		co_return RefreshResult;
	}
}
