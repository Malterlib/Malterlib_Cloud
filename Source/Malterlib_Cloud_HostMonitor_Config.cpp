// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

namespace NMib::NCloud
{
	using namespace NHostMonitorDatabase;
	using namespace NHostMonitor;

	TCFuture<void> CHostMonitor::CInternal::f_OpenConfigLogReporter()
	{
		if (m_ConfigLogReporter)
			co_return {};

		CDistributedAppLogReporter::CLogInfo LogInfo;
		LogInfo.m_Identifier = "org.malterlib.log.configmonitor";
		LogInfo.m_Name = "Malterlib Configuration Montior";
		LogInfo.m_MetaData = m_LogMetaData;

		m_ConfigLogReporter = co_await m_LogStore(&CDistributedAppLogStoreLocal::f_OpenLogReporter, fg_Move(LogInfo));

		co_return {};
	}

	TCFuture<CActorSubscription> CHostMonitor::f_MonitorConfigs(CDistributedAppInterfaceServer::CConfigFiles _ConfigFiles)
	{
		auto &Internal = *mp_pInternal;

		CStr ConfigID = NCryptography::fg_RandomID(Internal.m_MonitoredConfigs);
		CInternal::CMonitoredConfig MonitoredConfig;

		for (auto &ConfigFile : _ConfigFiles.m_Files)
		{
			auto &FileName = _ConfigFiles.m_Files.fs_GetKey(ConfigFile);
			auto &MonitoredFile = MonitoredConfig.m_Files[FileName];
			MonitoredFile.m_Options = ConfigFile;

			CFileChangeNotificationActor::CCoalesceSettings CoalesceSettings{.m_Delay = 0.0};

			CStr DirectoryPath = CFile::fs_GetPath(FileName);

			MonitoredFile.m_FileChangeSubscription = co_await Internal.m_FileChangeNotificationsActor
				(
					&CFileChangeNotificationActor::f_RegisterForChanges
					, DirectoryPath
					, EFileChange_All & ~EFileChange_Recursive
					, g_ActorFunctor / [this, ConfigID, FileName, DirectoryPath](TCVector<CFileChangeNotification::CNotification> _Notifications) -> TCFuture<void>
					{
						auto &Internal = *mp_pInternal;

						for (auto &Notification : _Notifications)
						{
							CStr File = DirectoryPath / Notification.m_Path;
							if (Notification.m_Notification == EFileChangeNotification_Renamed)
							{
								CStr FileFrom = DirectoryPath / Notification.m_PathFrom;
								if (File != FileName && FileFrom != FileName)
									continue;
							}
							else
							{
								if (File != FileName)
									continue;
							}

							Internal.f_ConfigFile_Changed(ConfigID, FileName) > fg_LogError("Malterlib/Cloud/HostMonitor", "Failed to process config file changed");
						}

						co_return {};
					}
					, CoalesceSettings
				)
			;
		}

		auto &CreatedConfig = Internal.m_MonitoredConfigs[ConfigID] = fg_Move(MonitoredConfig);

		TCFutureVector<void> InitialCheckResults;
		for (auto &ConfigFile : CreatedConfig.m_Files)
		{
			auto &FileName = CreatedConfig.m_Files.fs_GetKey(ConfigFile);
			Internal.f_ConfigFile_Changed(ConfigID, FileName) > InitialCheckResults;
		}

		if (auto Result = co_await fg_AllDone(InitialCheckResults).f_Wrap(); !Result)
			Result > fg_LogError("Malterlib/Cloud/HostMonitor", "Failed to check config file initially");

		co_return g_ActorSubscription / [this, ConfigID]() -> TCFuture<void>
			{
				auto &Internal = *mp_pInternal;

				auto *pConfig = Internal.m_MonitoredConfigs.f_FindEqual(ConfigID);
				if (!pConfig)
					co_return {};

				TCFutureVector<void> Results;

				for (auto &File : pConfig->m_Files)
					fg_Move(File.m_UpdateSequencer).f_Destroy() > Results;

				Internal.m_MonitoredConfigs.f_Remove(pConfig);

				co_await fg_AllDone(Results).f_Wrap() > fg_LogError("Malterlib/Cloud/HostMonitor", "Failed to destroy sequencers");

				co_return {};
			}
		;
	}

	CExceptionPointer CHostMonitor::CInternal::f_ConfigFile_CheckFilePrerequisites(CMonitoredConfigFile * &o_pConfigFile, CStr _ConfigID, CStr _FileName)
	{
		auto *pSubscription = m_MonitoredConfigs.f_FindEqual(_ConfigID);
		if (!pSubscription)
			return DMibErrorInstance("Config file subscription aborted");

		o_pConfigFile = pSubscription->m_Files.f_FindEqual(_FileName);
		if (!o_pConfigFile)
			return DMibErrorInstance("Config file subscription aborted");

		return {};
	}

	TCFuture<void> CHostMonitor::CInternal::f_ConfigFile_Changed(CStr _ConfigID, CStr _FileName)
	{
		CMonitoredConfigFile *pConfigFile = nullptr;
		auto OnResume = co_await fg_OnResume
			(
				[&]
				{
					return f_ConfigFile_CheckFilePrerequisites(pConfigFile, _ConfigID, _FileName);
				}
			)
		;

		auto Type = pConfigFile->m_Options.m_Type;

		auto SequenceSubscription = co_await pConfigFile->m_UpdateSequencer.f_Sequence();

		CConfigFileHistoryEntryValue Value;
		Value.m_Properties.m_UniqueProperties.m_ConfigType = Type;

		for (mint iRetry = 0; true; ++iRetry)
		{
			TCAsyncResult<CConfigFileHistoryEntryValue> Result;
			{
				auto BlockingActorCheckout = fg_BlockingActor();
				Result = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [Type, _FileName]() -> TCFuture<CConfigFileHistoryEntryValue>
						{
							auto CaptureScope = g_CaptureExceptions.f_Specific<CExceptionFile>();

							CConfigFileHistoryEntryValue Value;

							auto &UniqueProperties = Value.m_Properties.m_UniqueProperties;

							UniqueProperties.m_bExists = CFile::fs_FileExists(_FileName);

							if (!UniqueProperties.m_bExists)
								co_return {};

							Value.m_Contents.m_Raw = CFile::fs_ReadFile(_FileName);
							UniqueProperties.m_Size = CFile::fs_GetFileSize(_FileName);
							UniqueProperties.m_Digest = CHash_SHA256::fs_DigestFromData(Value.m_Contents.m_Raw);
							UniqueProperties.m_Attributes = CFile::fs_GetAttributesOnLink(_FileName);
							UniqueProperties.m_Owner = CFile::fs_GetOwner(_FileName);
							UniqueProperties.m_Group = CFile::fs_GetGroup(_FileName);

							switch (Type)
							{
							case CDistributedAppInterfaceServer::EMonitorConfigType_GeneralText:
								{
									CConfigFileContents_GeneralText Contents;
									Contents.m_Parsed = CFile::fs_ReadStringFromVector(Value.m_Contents.m_Raw, true);

									Value.m_Contents.m_Parsed = fg_Move(Contents);
								}
								break;
							case CDistributedAppInterfaceServer::EMonitorConfigType_GeneralBinary:
								{
									CConfigFileContents_GeneralBinary Contents;

									Value.m_Contents.m_Parsed = fg_Move(Contents);
								}
								break;
							case CDistributedAppInterfaceServer::EMonitorConfigType_Registry:
								{
									CConfigFileContents_Registry Contents;

									auto String = CFile::fs_ReadStringFromVector(Value.m_Contents.m_Raw, true);
									try
									{
										CRegistryPreserveAllFull Registry;
										Registry.f_ParseStr(String, _FileName);
										Contents.m_Parsed = fg_Move(Registry);
									}
									catch (CException const &_Exception)
									{
										UniqueProperties.m_ParseError = _Exception.f_GetErrorStr();
									}

									Value.m_Contents.m_Parsed = fg_Move(Contents);
								}
								break;
							case CDistributedAppInterfaceServer::EMonitorConfigType_Json:
								{
									CConfigFileContents_Json Contents;

									auto String = CFile::fs_ReadStringFromVector(Value.m_Contents.m_Raw, true);
									try
									{
										Contents.m_Parsed = CEJsonSorted::fs_FromString(String, _FileName, false, EJsonDialectFlag_AllowUndefined | EJsonDialectFlag_AllowInvalidFloat);
									}
									catch (CException const &_Exception)
									{
										UniqueProperties.m_ParseError = _Exception.f_GetErrorStr();
									}

									Value.m_Contents.m_Parsed = fg_Move(Contents);
								}
								break;
							}

							co_return fg_Move(Value);
						}
					)
					.f_Wrap()
				;
			}

			if (Result)
			{
				Value = fg_Move(*Result);
				break;
			}
			else if (iRetry >= 50)
				co_return Result.f_GetException();

			co_await fg_Timeout(0.1);
		}

		Value.m_Properties.m_Timestamp = CTime::fs_NowUTC();

		co_await f_ConfigFile_ValueChanged(_ConfigID, _FileName, fg_Move(Value));

		co_return {};
	}

	TCFuture<void> CHostMonitor::CInternal::f_ConfigFile_ValueChanged(CStr _ConfigID, CStr _FileName, CConfigFileHistoryEntryValue _Value)
	{
		CMonitoredConfigFile *pConfigFile = nullptr;
		auto OnResume = co_await fg_OnResume
			(
				[&]
				{
					return f_ConfigFile_CheckFilePrerequisites(pConfigFile, _ConfigID, _FileName);
				}
			)
		;

		TCOptional<CConfigFileKeyValue> OldKeyValue;
		{
			auto ReadTransaction = co_await m_Database(&CDatabaseActor::f_OpenTransactionRead);

			OldKeyValue = co_await fg_Move(ReadTransaction).f_BlockingDispatch
				(
					[_FileName](CDatabaseActor::CTransactionRead &&_ReadTransaction)
					{
						TCOptional<CConfigFileKeyValue> OldKeyValue;
						auto iConfigFile = _ReadTransaction.m_Transaction.f_ReadCursor(CConfigFileHistoryEntryKey::mc_Prefix, _FileName);
						if (iConfigFile.f_Last())
						{
							CConfigFileKeyValue Value;
							try
							{
								Value.m_Key = iConfigFile.f_Key<CConfigFileHistoryEntryKey>();
								Value.m_Value = iConfigFile.f_Value<CConfigFileHistoryEntryValue>();

								OldKeyValue = fg_Move(Value);
							}
							catch (CException const &_Exception)
							{
								DMibLogWithCategory
									(
										Malterlib/Cloud/HostMonitor
										, Error
										, "Failed to read old config value for file '{}' with value size {}: {}"
										, _FileName
										, iConfigFile.f_Value().m_Size
										, _Exception
									)
								;
							}
						}

						return OldKeyValue;
					}
					, "Error reading config file data when comparing changed config file"
				)
			;
		}

		if (OldKeyValue)
		{
			auto &OldValue = (*OldKeyValue).m_Value;

			if (OldValue.m_Properties.m_UniqueProperties == _Value.m_Properties.m_UniqueProperties && OldValue.m_Contents == _Value.m_Contents)
				co_return {};
		}

		if (OldKeyValue)
			co_await f_ConfigFile_LogChanges(_ConfigID, _FileName, (*OldKeyValue).m_Value, _Value);

		co_await m_Database
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [OldKeyValue, _FileName, _Value](CDatabaseActor::CTransactionWrite _Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					auto CaptureScope = co_await (g_CaptureExceptions % "Error saving config file data");

					// TODO: Handle _bCompacting

					auto WriteTransaction = fg_Move(_Transaction);

					co_await fg_ContinueRunningOnActor(WriteTransaction.f_Checkout());

					CConfigFileHistoryEntryKey Key{.m_VersionKey = {.m_FileName = _FileName}};

					if (OldKeyValue)
						Key.m_VersionKey.m_Sequence = OldKeyValue->m_Key.m_VersionKey.m_Sequence + 1;

					WriteTransaction.m_Transaction.f_Upsert(Key, _Value);

					co_return fg_Move(WriteTransaction);
				}
			)
		;

		co_return {};
	}

	namespace
	{
		void fg_FindJsonDiffsRecursive(CEJsonSorted const &_Old, CEJsonSorted const &_New, TCVector<CStr> &o_Changed, TCVector<CStr> &o_Added, TCVector<CStr> &o_Deleted, CStr const &_Context)
		{
			if (_Old == _New)
				return;

			if (_Old.f_EType() != _New.f_EType())
			{
				o_Changed.f_Insert(_Context);
				return;
			}
			switch (_New.f_EType())
			{
			case EEJsonType_Invalid:
			case EEJsonType_Null:
				break;
			case EEJsonType_String:
			case EEJsonType_Integer:
			case EEJsonType_Float:
			case EEJsonType_Boolean:
			case EEJsonType_Date:
			case EEJsonType_UserType:
			case EEJsonType_Binary:
				o_Changed.f_Insert(_Context);
				break;
			case EEJsonType_Object:
				{
					auto iOld = _Old.f_Object().f_OrderedIterator();
					auto iNew = _New.f_Object().f_OrderedIterator();
					while (iOld || iNew)
					{
						while (iOld && (!iNew || iOld->f_Name() < iNew->f_Name()))
						{
							o_Deleted.f_Insert("{}.{}"_f << _Context << iOld->f_Name());
							++iOld;
						}

						while (iNew && (!iOld || iNew->f_Name() < iOld->f_Name()))
						{
							o_Added.f_Insert("{}.{}"_f << _Context << iNew->f_Name());
							++iNew;
						}

						while (iOld && iNew && iNew->f_Name() == iOld->f_Name())
						{
							fg_FindJsonDiffsRecursive(iOld->f_Value(), iNew->f_Value(), o_Changed, o_Added, o_Deleted, "{}.{}"_f << _Context << iNew->f_Name());
							++iOld;
							++iNew;
						}
					}
				}
				break;
			case EEJsonType_Array:
				{
					auto iOld = _Old.f_Array().f_GetIterator();
					auto iNew = _New.f_Array().f_GetIterator();
					mint iArray = 0;
					for (;iOld && iNew; ++iOld, ++iNew, ++iArray)
						fg_FindJsonDiffsRecursive(*iOld, *iNew, o_Changed, o_Added, o_Deleted, "{}.[{}]"_f << _Context << iArray);

					for (;iOld ; ++iOld, ++iArray)
						o_Deleted.f_Insert("{}.[{}]"_f << _Context << iArray);
					for (;iNew ; ++iNew, ++iArray)
						o_Added.f_Insert("{}.[{}]"_f << _Context << iArray);
				}
				break;
			}
		}

		void fg_FindJsonDiffs(CEJsonSorted const &_Old, CEJsonSorted const &_New, TCVector<CStr> &o_Changed, TCVector<CStr> &o_Added, TCVector<CStr> &o_Deleted)
		{
			fg_FindJsonDiffsRecursive(_Old, _New, o_Changed, o_Added, o_Deleted, "<Root>");
		}
	}

	TCFuture<void> CHostMonitor::CInternal::f_ConfigFile_LogChanges
		(
			CStr _ConfigID
			, CStr _FileName
			, NHostMonitorDatabase::CConfigFileHistoryEntryValue _OldValue
			, NHostMonitorDatabase::CConfigFileHistoryEntryValue _NewValue
		)
	{
		CMonitoredConfigFile *pConfigFile = nullptr;
		auto OnResume = co_await fg_OnResume
			(
				[&]
				{
					return f_ConfigFile_CheckFilePrerequisites(pConfigFile, _ConfigID, _FileName);
				}
			)
		;

		if (!m_ConfigLogReporter)
			co_await f_OpenConfigLogReporter();

		auto &Reporter = *m_ConfigLogReporter;

		CDistributedAppLogReporter::CLogEntry LogEntry;
		LogEntry.m_Data.m_Severity = CDistributedAppLogReporter::ELogSeverity_Info;
		LogEntry.m_Data.m_Categories.f_Insert(_FileName);

		TCVector<CStr> MessageParagraphs;

		auto &OldProperties = _OldValue.m_Properties.m_UniqueProperties;
		auto &NewProperties = _NewValue.m_Properties.m_UniqueProperties;

		if (OldProperties.m_bExists && !NewProperties.m_bExists)
		{
			LogEntry.m_Data.m_Severity = CDistributedAppLogReporter::ELogSeverity_Error;
			MessageParagraphs.f_Insert("Config file no longer exists");
		}
		else if (!OldProperties.m_bExists && NewProperties.m_bExists)
			MessageParagraphs.f_Insert("Config file was previously deleted and now exists again");
		else if (NewProperties.m_bExists)
		{
			if (NewProperties.m_ParseError)
			{
				LogEntry.m_Data.m_Severity = CDistributedAppLogReporter::ELogSeverity_Error;
				MessageParagraphs.f_Insert("Failed to parse the config file: {}"_f << NewProperties.m_ParseError);
			}
			else if (OldProperties.m_ParseError)
				MessageParagraphs.f_Insert("Failing to parse the config file was resolved");

			if (OldProperties.m_Digest != NewProperties.m_Digest)
			{
				if (_OldValue.m_Contents.m_Parsed.f_GetTypeID() != _NewValue.m_Contents.m_Parsed.f_GetTypeID())
				{
					MessageParagraphs.f_Insert
						(
							"Type of file changed from '{}' to '{}'"_f
							<< CDistributedAppInterfaceServer::fs_MonitorConfigTypeToString(_OldValue.m_Contents.m_Parsed.f_GetTypeID())
							<< CDistributedAppInterfaceServer::fs_MonitorConfigTypeToString(_NewValue.m_Contents.m_Parsed.f_GetTypeID())
						)
					;
				}
				else if (_OldValue.m_Contents != _NewValue.m_Contents)
				{
					auto fAddChangedSection = [&](CStr const &_Description, TCVector<CStr> const &_List) mutable
						{
							if (_List.f_IsEmpty())
								return;

							MessageParagraphs.f_Insert("{}:\n{}"_f << _Description << CStr::fs_Join(_List, "\n"));
						}
					;

					switch (_NewValue.m_Contents.m_Parsed.f_GetTypeID())
					{
					case CDistributedAppInterfaceServer::EMonitorConfigType_GeneralText:
						{
							MessageParagraphs.f_Insert("General text contents changed");
						}
						break;
					case CDistributedAppInterfaceServer::EMonitorConfigType_GeneralBinary:
						{
							MessageParagraphs.f_Insert("General binary contents changed");
						}
						break;
					case CDistributedAppInterfaceServer::EMonitorConfigType_Registry:
						{
							auto &Old = _OldValue.m_Contents.m_Parsed.f_GetAsType<CConfigFileContents_Registry>();
							auto &New = _NewValue.m_Contents.m_Parsed.f_GetAsType<CConfigFileContents_Registry>();

							MessageParagraphs.f_Insert("Registry contents changed:");

							if (!NewProperties.m_ParseError)
							{
								TCVector<CStr> Changed;
								TCVector<CStr> Added;
								TCVector<CStr> Deleted;
								New.m_Parsed.f_FindDiffs(Old.m_Parsed, Changed, Added, Deleted);

								fAddChangedSection("Added keys", Added);
								fAddChangedSection("Removed keys", Deleted);
								fAddChangedSection("Changed keys", Changed);
							}
						}
						break;
					case CDistributedAppInterfaceServer::EMonitorConfigType_Json:
						{
							auto &Old = _OldValue.m_Contents.m_Parsed.f_GetAsType<CConfigFileContents_Json>();
							auto &New = _NewValue.m_Contents.m_Parsed.f_GetAsType<CConfigFileContents_Json>();

							MessageParagraphs.f_Insert("JSON contents changed:");

							if (!NewProperties.m_ParseError)
							{
								TCVector<CStr> Changed;
								TCVector<CStr> Added;
								TCVector<CStr> Deleted;
								fg_FindJsonDiffs(Old.m_Parsed, New.m_Parsed, Changed, Added, Deleted);

								fAddChangedSection("Added paths", Added);
								fAddChangedSection("Removed paths", Deleted);
								fAddChangedSection("Changed paths", Changed);
							}
						}
						break;
					}
				}
				else
					MessageParagraphs.f_Insert("File contents changed, but parsed contents stayed the same");
			}
		}

		if (OldProperties.m_Owner != NewProperties.m_Owner)
			MessageParagraphs.f_Insert("Owner changed from '{}' to '{}'"_f << OldProperties.m_Owner << NewProperties.m_Owner);

		if (OldProperties.m_Group != NewProperties.m_Group)
			MessageParagraphs.f_Insert("Group changed from '{}' to '{}'"_f << OldProperties.m_Group << NewProperties.m_Group);

		if (OldProperties.m_Size != NewProperties.m_Size)
			MessageParagraphs.f_Insert("File size changed from {ns } to {ns }"_f << OldProperties.m_Size << NewProperties.m_Size);

		if (OldProperties.m_Attributes != NewProperties.m_Attributes)
		{
			MessageParagraphs.f_Insert
				(
					"Attributes changed:\n{} to\n{}"_f
					<< CFile::fs_AttribToJson(OldProperties.m_Attributes).f_ToString(nullptr)
					<< CFile::fs_AttribToJson(NewProperties.m_Attributes).f_ToString(nullptr)
				)
			;
		}

		MessageParagraphs.f_Insert("Old timestamp: {tc6}\nNew timestamp: {tc6}"_f << _OldValue.m_Properties.m_Timestamp << _NewValue.m_Properties.m_Timestamp);

		LogEntry.m_Data.m_Message = CStr::fs_Join(MessageParagraphs, "\n\n");

		co_await Reporter.m_fReportEntries(TCVector{fg_Move(LogEntry)});

		co_return {};
	}

	TCFuture<TCSet<CStr>> CHostMonitor::f_EnumConfigFiles()
	{
		auto &Internal = *mp_pInternal;

		auto ReadTransaction = co_await Internal.m_Database(&CDatabaseActor::f_OpenTransactionRead);

		co_return co_await fg_Move(ReadTransaction).f_BlockingDispatch
			(
				[](CDatabaseActor::CTransactionRead &&_ReadTransaction)
				{
					TCSet<CStr> ConfigFiles;

					CStr LastFile;
					for (auto iConfigFile = _ReadTransaction.m_Transaction.f_ReadCursor(CConfigFileHistoryEntryKey::mc_Prefix); iConfigFile; ++iConfigFile)
					{
						auto Key = iConfigFile.f_Key<CConfigFileHistoryEntryKey>();

						if (LastFile == Key.m_VersionKey.m_FileName)
							continue;
						LastFile = Key.m_VersionKey.m_FileName;

						ConfigFiles[Key.m_VersionKey.m_FileName];
					}

					return ConfigFiles;
				}
				, "Error reading from database"
			)
		;
	}

	TCFuture<TCMap<CConfigFileVersionKey, CConfigFileProperties>> CHostMonitor::f_EnumConfigFileVersions(CStr _File)
	{
		auto &Internal = *mp_pInternal;

		auto ReadTransaction = co_await Internal.m_Database(&CDatabaseActor::f_OpenTransactionRead);

		co_return co_await fg_Move(ReadTransaction).f_BlockingDispatch
			(
				[_File](CDatabaseActor::CTransactionRead &&_ReadTransaction)
				{
					TCMap<CConfigFileVersionKey, CConfigFileProperties> ConfigFileVersions;

					for (auto iConfigFile = _ReadTransaction.m_Transaction.f_ReadCursor(CConfigFileHistoryEntryKey::mc_Prefix, _File); iConfigFile; ++iConfigFile)
					{
						auto Key = iConfigFile.f_Key<CConfigFileHistoryEntryKey>();
						auto Value = iConfigFile.f_Value<CConfigFileHistoryEntryValue>();

						ConfigFileVersions[fg_Move(Key.m_VersionKey)] = fg_Move(Value.m_Properties);
					}

					return ConfigFileVersions;
				}
				, "Error reading from database"
			)
		;
	}

	TCFuture<CConfigFileContents> CHostMonitor::f_GetConfigFileContents(CConfigFileVersionKey _Key)
	{
		auto &Internal = *mp_pInternal;

		auto ReadTransaction = co_await Internal.m_Database(&CDatabaseActor::f_OpenTransactionRead);

		co_return co_await fg_Move(ReadTransaction).f_BlockingDispatch
			(
				[_Key = fg_Move(_Key)](CDatabaseActor::CTransactionRead _ReadTransaction) mutable -> TCFuture<CConfigFileContents>
				{
					CConfigFileHistoryEntryValue Value;
					if (_Key.m_Sequence == TCLimitsInt<uint64>::mc_Max)
					{
						auto iConfigFile = _ReadTransaction.m_Transaction.f_ReadCursor(CConfigFileHistoryEntryKey::mc_Prefix, _Key.m_FileName);
						if (!iConfigFile.f_Last())
							co_return DMibErrorInstance("File does not exist in database");

						Value = iConfigFile.f_Value<CConfigFileHistoryEntryValue>();
					}
					else
					{
						CConfigFileHistoryEntryKey Key = {.m_VersionKey = fg_Move(_Key)};

						if (!_ReadTransaction.m_Transaction.f_Get(Key, Value))
							co_return DMibErrorInstance("Version does not exist in database");
					}

					if (!Value.m_Properties.m_UniqueProperties.m_bExists)
						co_return DMibErrorInstance("The config file does not exist");

					co_return fg_Move(Value.m_Contents);
				}
				, "Error reading from database"
			)
		;
	}
}
