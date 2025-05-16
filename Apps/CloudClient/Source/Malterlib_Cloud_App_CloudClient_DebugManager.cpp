// Copyright © 2025 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include <Mib/Cloud/DebugManager>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Daemon/Daemon>
#include <Mib/Encoding/JsonShortcuts>

#include "Malterlib_Cloud_App_CloudClient.h"

#ifdef DPlatformFamily_Windows
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#endif

namespace NMib::NCloud::NCloudClient
{
	namespace
	{
		CDebugManager::CMetadata fg_ParseMetadataOptions(CEJsonSorted const &_Params)
		{
			CDebugManager::CMetadata Return;

			if (auto pValue = _Params.f_GetMember("Metadata_Product"))
				Return.m_Product = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("Metadata_Application"))
				Return.m_Application = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("Metadata_Configuration"))
				Return.m_Configuration = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("Metadata_GitBranch"))
				Return.m_GitBranch = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("Metadata_GitCommit"))
				Return.m_GitCommit = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("Metadata_Platform"))
				Return.m_Platform = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("Metadata_Version"))
				Return.m_Version = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("Metadata_Tags"))
				Return.m_Tags = TCSet<CStr>::fs_FromContainer(pValue->f_StringArray());

			return Return;
		}

		CDebugManager::CAssetFilter fg_ParseAssetFilter(CEJsonSorted const &_Params)
		{
			CDebugManager::CAssetFilter Filter;

			if (auto pValue = _Params.f_GetMember("AssetType"))
				Filter.m_AssetType = CDebugManager::fs_AssetTypeFromStr(pValue->f_String());

			if (auto pValue = _Params.f_GetMember("BuildID"))
				Filter.m_BuildID = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("FileName"))
				Filter.m_FileName = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("TimestampStart"))
				Filter.m_TimestampStart = pValue->f_Date();

			if (auto pValue = _Params.f_GetMember("TimestampEnd"))
				Filter.m_TimestampEnd = pValue->f_Date();

			Filter.m_Metadata = fg_ParseMetadataOptions(_Params);

			return Filter;
		}

		CDebugManager::CCrashDumpFilter fg_ParseCrashDumpFilter(CEJsonSorted const &_Params)
		{
			CDebugManager::CCrashDumpFilter Filter;

			if (auto pValue = _Params.f_GetMember("ID"))
				Filter.m_ID = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("FileName"))
				Filter.m_FileName = pValue->f_String();

			if (auto pValue = _Params.f_GetMember("TimestampStart"))
				Filter.m_TimestampStart = pValue->f_Date();

			if (auto pValue = _Params.f_GetMember("TimestampEnd"))
				Filter.m_TimestampEnd = pValue->f_Date();

			if (auto pValue = _Params.f_GetMember("ExceptionInfo"))
				Filter.m_ExceptionInfo = pValue->f_String();			

			Filter.m_Metadata = fg_ParseMetadataOptions(_Params);

			return Filter;
		}
	}

	void CCloudClientAppActor::fp_DebugManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		auto DebugManagerHost = "DebugManagerHost?"_o=
			{
				"Names"_o= _o["--host"]
				, "Default"_o= ""
				, "Description"_o= "Limit query to only specified host ID."
			}
		;
		auto IncludeHost = "IncludeHost?"_o=
			{
				"Names"_o= _o["--include-host"]
				, "Default"_o= false
				, "Description"_o= "Include debug manager host in output.\n"
			}
		;
		auto UploadDownloadHostOption = "DebugManagerHost?"_o=
			{
				"Names"_o= _o["--host"]
				, "Default"_o= ""
				, "Description"_o= "The host ID of the host to perform transfer on."
			}
		;
		auto UploadForceOption = "Force?"_o=
			{
				"Names"_o= _o["--force"]
				, "Default"_o= false
				, "Description"_o= "Force upload even if version already exists.\n"
			}
		;
		auto TransferQueueSizeOption = "TransferQueueSize?"_o=
			{
				"Names"_o= _o["--queue-size"]
				, "Default"_o= int64(NFile::gc_IdealNetworkQueueSize)
				, "Description"_o= "The amount of data to keep in flight while transferring data."
			}
		;
		auto CompressionLevelOption = "CompressionLevel?"_o=
			{
				"Names"_o= _o["--compression-level"]
				, "Default"_o= int64(8)
				, "Description"_o= "Which compression level to use for the Zstandard compression."
			}
		;
		auto HiddenCurrentDirectoryOption = "CurrentDirectory?"_o=
			{
				"Names"_o= _o[]
				, "Default"_o= CFile::fs_GetCurrentDirectory()
				, "Hidden"_o= true
				, "Description"_o= "Internal hidden option to forward current directory."
			}
		;
		auto fAssetTypeOption = [](bool _bOptional)
			{
				return (_bOptional ? "AssetType?"_o : "AssetType"_o)=
					{
						"Names"_o= _o["--asset-type", "-t"]
						, "Type"_o= COneOf("Executable", "DebugInfo")
						, "Description"_o= "The type of asset.\n"
					}
				;
			}
		;
		auto fBuildIDOption = [](bool _bOptional)
			{
				return (_bOptional ? "BuildID?"_o : "BuildID"_o)=
					{
						"Names"_o= _o["--build-id", "-i"]
						,"Type"_o= ""
						, "Description"_o= "The build ID.\n"
					}
				;
			}
		;
		auto fCrashDumpIDOption = [](bool _bOptional)
			{
				return (_bOptional ? "ID?"_o : "ID"_o)=
					{
						"Names"_o= _o["--id", "-i"]
						,"Type"_o= ""
						, "Description"_o= "The crash dump ID.\n"
					}
				;
			}
		;
		auto MetadataOption_Product = "Metadata_Product?"_o=
			{
				"Names"_o= _o["--metadata-product"]
				, "Type"_o= ""
				, "Description"_o= "Product metadata."
			}
		;
		auto MetadataOption_Application = "Metadata_Application?"_o=
			{
				"Names"_o= _o["--metadata-application"]
				, "Type"_o= ""
				, "Description"_o= "Application metadata."
			}
		;
		auto MetadataOption_Configuration = "Metadata_Configuration?"_o=
			{
				"Names"_o= _o["--metadata-configuration"]
				, "Type"_o= ""
				, "Description"_o= "Configuration metadata."
			}
		;
		auto MetadataOption_GitBranch = "Metadata_GitBranch?"_o=
			{
				"Names"_o= _o["--metadata-git-branch"]
				, "Type"_o= ""
				, "Description"_o= "Git branch metadata."
			}
		;
		auto MetadataOption_GitCommit = "Metadata_GitCommit?"_o=
			{
				"Names"_o= _o["--metadata-git-commit"]
				, "Type"_o= ""
				, "Description"_o= "Git commit metadata."
			}
		;
		auto MetadataOption_Platform = "Metadata_Platform?"_o=
			{
				"Names"_o= _o["--metadata-platform"]
				, "Type"_o= ""
				, "Description"_o= "Platform metadata."
			}
		;
		auto MetadataOption_Version = "Metadata_Version?"_o=
			{
				"Names"_o= _o["--metadata-version"]
				, "Type"_o= ""
				, "Description"_o= "Version metadata."
			}
		;
		auto MetadataOption_Tags = "Metadata_Tags?"_o=
			{
				"Names"_o= _o["--metadata-tags"]
				, "Type"_o= _o[""]
				, "Description"_o= "Tags metadata."
			}
		;

		auto FilterFileNameOption = "FileName?"_o=
			{
				"Names"_o= _o["--file-name", "-n"]
				,"Type"_o= ""
				, "Description"_o= "File name pattern to match.\n"
			}
		;
		auto FilterTimestampStartOption = "TimestampStart?"_o=
			{
				"Names"_o= _o["--timestamp-start"]
				,"Type"_o= CTime()
				, "Description"_o= "The start of the timestamp range to match.\n"
			}
		;
		auto FilterTimestampEndOption = "TimestampEnd?"_o=
			{
				"Names"_o= _o["--timestamp-end"]
				,"Type"_o= CTime()
				, "Description"_o= "The end of the timestamp range to match.\n"
			}
		;
		auto FilterExceptionInfoOption = "ExceptionInfo?"_o=
			{
				"Names"_o= _o["--exception-info"]
				,"Type"_o= ""
				, "Description"_o= "Exception info pattern to match.\n"
			}
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--debug-manager-asset-list"]
					, "Description"_o= "List assets available on remote debug managers."
					, "Options"_o=
					{
						DebugManagerHost
						, IncludeHost
						, "Verbosity?"_o=
						{
							"Names"_o= _o["--verbosity", "-v"]
							,"Default"_o= 2
							, "Description"_o= "Output verbosity.\n"
						}
						, fAssetTypeOption(true)
						, fBuildIDOption(true)
						, FilterFileNameOption
						, FilterTimestampStartOption
						, FilterTimestampEndOption
						, MetadataOption_Product
						, MetadataOption_Application
						, MetadataOption_Configuration
						, MetadataOption_GitBranch
						, MetadataOption_GitCommit
						, MetadataOption_Platform
						, MetadataOption_Version
						, MetadataOption_Tags
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DebugManager_DebugAssetList(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--debug-manager-asset-delete"]
					, "Description"_o= "List assets available on remote debug managers."
					, "Options"_o=
					{
						DebugManagerHost
						, "Pretend?"_o=
						{
							"Names"_o= _o["--pretend"]
							,"Default"_o= true
							, "Description"_o= "Check how much would be deleted without actually deleting anything.\n"
						}
						, "AllowMultiDelete?"_o=
						{
							"Names"_o= _o["--allow-multi-delete", "-f"]
							,"Default"_o= false
							, "Description"_o= "Allow deleting several assets at the same time.\n"
						}
						, "MaxDeletes?"_o=
						{
							"Names"_o= _o["--max-deletions", "-m"]
							,"Default"_o= 1
							, "Description"_o= "Max number of assets to delete. To avoid mistakes this defaults to 1.\n"
						}
						, fAssetTypeOption(true)
						, fBuildIDOption(true)
						, FilterFileNameOption
						, FilterTimestampStartOption
						, FilterTimestampEndOption
						, MetadataOption_Product
						, MetadataOption_Application
						, MetadataOption_Configuration
						, MetadataOption_GitBranch
						, MetadataOption_GitCommit
						, MetadataOption_Platform
						, MetadataOption_Version
						, MetadataOption_Tags
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DebugManager_DebugAssetDelete(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--debug-manager-asset-upload"]
					, "Description"_o= "Upload a debug asset to remote debug manager.\n"
					, "Options"_o=
					{
						fAssetTypeOption(false)
						, UploadDownloadHostOption
						, UploadForceOption
						, TransferQueueSizeOption
						, CompressionLevelOption
						, MetadataOption_Product
						, MetadataOption_Application
						, MetadataOption_Configuration
						, MetadataOption_GitBranch
						, MetadataOption_GitCommit
						, MetadataOption_Platform
						, MetadataOption_Version
						, MetadataOption_Tags
						, HiddenCurrentDirectoryOption
					}
					, "Parameters"_o=
					{
						"Source"_o=
						{
							"Default"_o= ""
							, "Description"_o= "The file or directory to upload.\n"
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DebugManager_DebugAssetUpload(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--debug-manager-asset-download"]
					, "Description"_o= "Download a version from remote debug manager.\n"
					, "Options"_o=
					{
						UploadDownloadHostOption
						, TransferQueueSizeOption
						, "AllowMultiDownload?"_o=
						{
							"Names"_o= _o["--allow-multi-download", "-f"]
							,"Default"_o= false
							, "Description"_o= "Allow downloading several build IDs at the same time.\n"
						}
						, fAssetTypeOption(true)
						, fBuildIDOption(true)
						, FilterFileNameOption
						, FilterTimestampStartOption
						, FilterTimestampEndOption
						, MetadataOption_Product
						, MetadataOption_Application
						, MetadataOption_Configuration
						, MetadataOption_GitBranch
						, MetadataOption_GitCommit
						, MetadataOption_Platform
						, MetadataOption_Version
						, MetadataOption_Tags
						, HiddenCurrentDirectoryOption
					}
					, "Parameters"_o=
					{
						"DestinationDirectory?"_o=
						{
							"Default"_o= "."
							, "Description"_o= "The directory to download the version to."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DebugManager_DebugAssetDownload(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;

		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--debug-manager-crash-dump-list"]
					, "Description"_o= "List crash dumps available on remote debug managers."
					, "Options"_o=
					{
						DebugManagerHost
						, IncludeHost
						, "IncludeExceptionInfo?"_o=
						{
							"Names"_o= _o["--include-exception-info"]
							, "Default"_o= false
							, "Description"_o= "Include exception info in output.\n"
						}
						, "Verbosity?"_o=
						{
							"Names"_o= _o["--verbosity", "-v"]
							,"Default"_o= 2
							, "Description"_o= "Output verbosity.\n"
						}
						, fCrashDumpIDOption(true)
						, FilterFileNameOption
						, FilterTimestampStartOption
						, FilterTimestampEndOption
						, FilterExceptionInfoOption
						, MetadataOption_Product
						, MetadataOption_Application
						, MetadataOption_Configuration
						, MetadataOption_GitBranch
						, MetadataOption_GitCommit
						, MetadataOption_Platform
						, MetadataOption_Version
						, MetadataOption_Tags
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DebugManager_CrashDumpList(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--debug-manager-crash-dump-delete"]
					, "Description"_o= "List crash dumps available on remote debug managers."
					, "Options"_o=
					{
						DebugManagerHost
						, "Pretend?"_o=
						{
							"Names"_o= _o["--pretend"]
							,"Default"_o= true
							, "Description"_o= "Check how much would be deleted without actually deleting anything.\n"
						}
						, "AllowMultiDelete?"_o=
						{
							"Names"_o= _o["--allow-multi-delete", "-f"]
							,"Default"_o= false
							, "Description"_o= "Allow deleting several crash dumps at the same time.\n"
						}
						, "MaxDeletes?"_o=
						{
							"Names"_o= _o["--max-deletions", "-m"]
							,"Default"_o= 1
							, "Description"_o= "Max number of crash dumps to delete. To avoid mistakes this defaults to 1.\n"
						}
						, fCrashDumpIDOption(true)
						, FilterFileNameOption
						, FilterTimestampStartOption
						, FilterTimestampEndOption
						, FilterExceptionInfoOption
						, MetadataOption_Product
						, MetadataOption_Application
						, MetadataOption_Configuration
						, MetadataOption_GitBranch
						, MetadataOption_GitCommit
						, MetadataOption_Platform
						, MetadataOption_Version
						, MetadataOption_Tags
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DebugManager_CrashDumpDelete(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--debug-manager-crash-dump-upload"]
					, "Description"_o= "Upload a debug crash dump to remote debug manager.\n"
					, "Options"_o=
					{
						fCrashDumpIDOption(true)
						, UploadDownloadHostOption
						, UploadForceOption
						, TransferQueueSizeOption
						, CompressionLevelOption
						, MetadataOption_Product
						, MetadataOption_Application
						, MetadataOption_Configuration
						, MetadataOption_GitBranch
						, MetadataOption_GitCommit
						, MetadataOption_Platform
						, MetadataOption_Version
						, MetadataOption_Tags
						, HiddenCurrentDirectoryOption
					}
					, "Parameters"_o=
					{
						"Sources...?"_o=
						{
							"Type"_o= _o[""]
							, "Default"_o= _o[]
							, "Description"_o= "Specify the files and directories to upload crash dumps from."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DebugManager_CrashDumpUpload(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--debug-manager-crash-dump-download"]
					, "Description"_o= "Download a version from remote debug manager.\n"
					, "Options"_o=
					{
						UploadDownloadHostOption
						, TransferQueueSizeOption
						, "AllowMultiDownload?"_o=
						{
							"Names"_o= _o["--allow-multi-download", "-f"]
							,"Default"_o= false
							, "Description"_o= "Allow downloading several build IDs at the same time.\n"
						}
						, fCrashDumpIDOption(true)
						, FilterFileNameOption
						, FilterTimestampStartOption
						, FilterTimestampEndOption
						, FilterExceptionInfoOption
						, MetadataOption_Product
						, MetadataOption_Application
						, MetadataOption_Configuration
						, MetadataOption_GitBranch
						, MetadataOption_GitCommit
						, MetadataOption_Platform
						, MetadataOption_Version
						, MetadataOption_Tags
						, HiddenCurrentDirectoryOption
					}
					, "Parameters"_o=
					{
						"DestinationDirectory?"_o=
						{
							"Default"_o= "."
							, "Description"_o= "The directory to download the version to."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DebugManager_CrashDumpDownload(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}

	TCFuture<void> CCloudClientAppActor::fp_DebugManager_SubscribeToServers()
	{
		if (!mp_DebugManagers.f_IsEmpty())
			co_return {};
		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to debug managers");
		
		auto Subscription = co_await mp_State.m_TrustManager->f_SubscribeTrustedActors<NCloud::CDebugManager>().f_Wrap();

		if (!Subscription)
		{
			DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscribe to debug managers: {}", Subscription.f_GetExceptionStr());
			co_return Subscription.f_GetException();
		}

		mp_DebugManagers = fg_Move(*Subscription);

		if (mp_DebugManagers.m_Actors.f_IsEmpty())
			co_return DMibErrorInstance("Not connected to any debug managers, or they are not trusted for 'com.malterlib/Cloud/DebugManager' namespace");

		co_return {};
	}

	TCFuture<TCVector<TCTrustedActor<CDebugManager>>> CCloudClientAppActor::fp_DebugManager_GetDebugManagers(CStr _Host)
	{
		if (!_Host.f_IsEmpty())
		{
			CStr Error;
			auto *pDebugManager = mp_DebugManagers.f_GetOneActor(_Host, Error);
			if (!pDebugManager)
				co_return DMibErrorInstance("Error selecting debug manager for host '{}': {}. Connection might have failed. Use --log-to-stderr to see more info."_f << _Host << Error);

			co_return {*pDebugManager};
		}

		TCVector<TCTrustedActor<CDebugManager>> Return;

		for (auto &Actor : mp_DebugManagers.m_Actors)
			Return.f_Insert(Actor);

		if (Return.f_IsEmpty())
			co_return DMibErrorInstance("No debug managers found. Connection might have failed. Use --log-to-stderr to see more info.");

		co_return fg_Move(Return);
	}

	namespace
	{
		CStr fg_FormatBytes(uint64 _Bytes)
		{
			if (_Bytes >= 1024 * 1024 * 1024)
				return "{fe2} GiB"_f << (fp64(_Bytes) / fp64(1024 * 1024 * 1024));
			else if (_Bytes >= 1024*1024)
				return "{fe2} MiB"_f << (fp64(_Bytes) / fp64(1024 * 1024));
			else if (_Bytes >= 1024)
				return "{fe2} KiB"_f << (fp64(_Bytes) / fp64(1024));
			else
				return "{} B"_f << _Bytes;
		}
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_DebugManager_DebugAssetList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Host = _Params["DebugManagerHost"].f_String();
		bool bIncludeHost = _Params["IncludeHost"].f_Boolean();

		co_await fp_DebugManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for debug managers");

		auto Filter = fg_ParseAssetFilter(_Params);

		TCFutureMap<CHostInfo, CDebugManager::CAssetList::CResult> ListResults;
		for (auto &TrustedActor : mp_DebugManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;
			TrustedActor.m_Actor.f_CallActor(&CDebugManager::f_Asset_List)(CDebugManager::CAssetList{.m_Filter = Filter})
				.f_Timeout(mp_Timeout, "Timed out waiting for debug manager to reply")
				> ListResults[TrustedActor.m_TrustInfo.m_HostInfo]
			;
		}

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();

		CTableRenderHelper::CColumnHelper Columns(_Params["Verbosity"].f_Integer());

		Columns.f_AddHeading("Host", bIncludeHost ? 2 : 10);
		Columns.f_AddHeading("Asset Type", 2);
		Columns.f_AddHeading("Build ID", 0);
		Columns.f_AddHeading("File Name", 1);
		Columns.f_AddHeading("Main Asset", 4);
		Columns.f_AddHeading("Timestamp", 3);
		Columns.f_AddHeading("Digest", 3);
		Columns.f_AddHeading("Size", 3);
		Columns.f_AddHeading("Compressed", 4);
		Columns.f_AddHeading("Ratio", 4);
		Columns.f_AddHeading("Product", 6);
		Columns.f_AddHeading("Application", 2);
		Columns.f_AddHeading("Config", 5);
		Columns.f_AddHeading("Git Branch", 4);
		Columns.f_AddHeading("Git Commit", 7);
		Columns.f_AddHeading("Platform", 5);
		Columns.f_AddHeading("Version", 4);
		Columns.f_AddHeading("Tags", 4);

		Columns.f_SetSortByColumns({"Timestamp"});

		TableRenderer.f_AddHeadings(&Columns);
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);

		auto Results = co_await fg_AllDoneWrapped(ListResults);
		for (auto &Result : Results)
		{
			auto &HostInfo = Results.fs_GetKey(Result);
			CStr HostDescription = HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags);
			if (!Result)
			{
				*_pCommandLine %= "{}Failed getting applications for host{} '{}': {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					<< Result.f_GetExceptionStr()
				;
				continue;
			}

			for (auto iAssets = co_await fg_Move(Result->m_AssetsGenerator).f_GetPipelinedIterator(); iAssets; co_await ++iAssets)
			{
				for (auto &Asset : *iAssets)
				{
					TableRenderer.f_AddRow
						(
							HostDescription
							, CDebugManager::fs_AssetTypeToStr(Asset.m_AssetType)
							, Asset.m_BuildID
							, Asset.m_FileInfo.m_FileName
							, Asset.m_MainAssetFile
							, Asset.m_FileInfo.m_Timestamp
							, Asset.m_FileInfo.m_Digest.f_GetString()
							, fg_FormatBytes(Asset.m_FileInfo.m_Size)
							, fg_FormatBytes(Asset.m_FileInfo.m_CompressedSize)
							, "{fe1}"_f << (fp64(Asset.m_FileInfo.m_Size) / fp64(Asset.m_FileInfo.m_CompressedSize))
							, Asset.m_Metadata.m_Product ? *Asset.m_Metadata.m_Product : CStr()
							, Asset.m_Metadata.m_Application ? *Asset.m_Metadata.m_Application : CStr()
							, Asset.m_Metadata.m_Configuration ? *Asset.m_Metadata.m_Configuration : CStr()
							, Asset.m_Metadata.m_GitBranch ? *Asset.m_Metadata.m_GitBranch : CStr()
							, Asset.m_Metadata.m_GitCommit ? *Asset.m_Metadata.m_GitCommit : CStr()
							, Asset.m_Metadata.m_Platform ? *Asset.m_Metadata.m_Platform : CStr()
							, Asset.m_Metadata.m_Version ? *Asset.m_Metadata.m_Version : CStr()
							, Asset.m_Metadata.m_Tags ? "{vs}"_f << *Asset.m_Metadata.m_Tags : CStr()
						)
					;
				}
			}
		}

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_DebugManager_DebugAssetUpload(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Source = CFile::fs_GetExpandedPath(_Params["Source"].f_String(), _Params["CurrentDirectory"].f_String());
		if (Source.f_IsEmpty())
			co_return DMibErrorInstance("Source must be specified");

		CStr Host = _Params["DebugManagerHost"].f_String();
		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		int32 CompressionLevel = _Params["CompressionLevel"].f_Integer();
		bool bForce = _Params["Force"].f_Boolean();
		auto AssetType = CDebugManager::fs_AssetTypeFromStr(_Params["AssetType"].f_String());
		auto Metadata = fg_ParseMetadataOptions(_Params);

		if (QueueSize < 128 * 1024)
			QueueSize = 128 * 1024;

		auto FormatFlags = _pCommandLine->f_AnsiEncoding().f_Flags();

		co_await fp_DebugManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for debug managers");

		auto DebugManagers = co_await fp_DebugManager_GetDebugManagers(Host);

		TCFutureMap<CHostInfo, CDebugManagerHelper::CUploadResult> Results;

		for (auto &DebugManager : DebugManagers)
		{
			mp_DebugManagerHelper.f_Asset_Upload
				(
					DebugManager.m_Actor
					, Source
					, AssetType
					, Metadata
					, bForce ? CDebugManager::EUploadFlag::mc_ForceOverwrite : CDebugManager::EUploadFlag::mc_None
					, QueueSize
					, CompressionLevel
				)
				> Results[DebugManager.m_TrustInfo.m_HostInfo]
			;
		}

		for (auto &Result : (co_await fg_AllDoneWrapped(Results)).f_Entries())
		{
			if (!Result.f_Value())
			{
				*_pCommandLine %= "Failed to upload to '{}': {}\n"_f << Result.f_Key().f_GetDescColored(FormatFlags) << Result.f_Value().f_GetExceptionStr();
				continue;
			}

			auto &UploadResult = *Result.f_Value();

			*_pCommandLine += "Upload finished transferring to '{}': {ns } bytes at {fe2} MB/s\n"_f
				<< Result.f_Key().f_GetDescColored(FormatFlags)
				<< UploadResult.m_TransferResult.m_nBytes
				<< UploadResult.m_TransferResult.f_BytesPerSecond() / 1'000'000.0
			;
		}

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_DebugManager_DebugAssetDownload(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Host = _Params["DebugManagerHost"].f_String();
		CStr DestinationDirectory = CFile::fs_GetExpandedPath(_Params["DestinationDirectory"].f_String(), _Params["CurrentDirectory"].f_String());
		if (DestinationDirectory.f_IsEmpty())
			co_return DMibErrorInstance("Destination directory must be specified");

		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		bool bAllowMultiDownload = _Params["AllowMultiDownload"].f_Boolean();
		auto Filter = fg_ParseAssetFilter(_Params);

		if (!bAllowMultiDownload && !Filter.m_BuildID)
			co_return DMibErrorInstance("Your filtering options could match multiple build IDs. If you want to allow this use the -f flag.");

		TCOptional<CStr> FileName;
		if (auto pValue = _Params.f_GetMember("FileName"))
			FileName = pValue->f_String();

		if (QueueSize < 128 * 1024)
			QueueSize = 128 * 1024;

		auto FormatFlags = _pCommandLine->f_AnsiEncoding().f_Flags();

		co_await fp_DebugManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for debug managers");

		auto DebugManagers = co_await fp_DebugManager_GetDebugManagers(Host);

		CExceptionExceptionVectorData::CErrorCollector Collector;

		for (auto &DebugManager : DebugManagers)
		{
			auto Results = co_await mp_DebugManagerHelper.f_Asset_Download
				(
					DebugManager.m_Actor
					, Filter
					, DestinationDirectory
					, CFileTransferReceive::EReceiveFlag_None
					, QueueSize
				)
				.f_Wrap()
			;

			if (!Results)
			{
				Collector.f_AddError
					(
						DMibErrorInstanceWrapped
						(
							"Failed to download from '{}': {}"_f << DebugManager.m_TrustInfo.m_HostInfo.f_GetDescColored(FormatFlags) << Results.f_GetExceptionStr()
							, Results.f_ExceptionPointer()
						)
					)
				;
				continue;
			}

			*_pCommandLine += "Download finished transferring from '{}': {ns } bytes at {fe2} MB/s\n"_f
				<< DebugManager.m_TrustInfo.m_HostInfo.f_GetDescColored(FormatFlags)
				<< Results->m_nBytes
				<< Results->f_BytesPerSecond() / 1'000'000.0
			;

			co_return 0;
		}

		if (Collector.f_HasError())
			co_return fg_Move(Collector).f_GetException();

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_DebugManager_DebugAssetDelete(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Host = _Params["DebugManagerHost"].f_String();
		uint64 nMaxToDelete = _Params["MaxDeletes"].f_Integer();
		bool bAllowMultiDelete = _Params["AllowMultiDelete"].f_Boolean();
		bool bPretend = _Params["Pretend"].f_Boolean();
		auto Filter = fg_ParseAssetFilter(_Params);

		if (!bAllowMultiDelete && !Filter.m_BuildID)
			co_return DMibErrorInstance("Your filtering options could match multiple build IDs. If you want to allow this use the -f flag.");

		co_await fp_DebugManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for debug managers");

		CDebugManager::CAssetDelete DeleteParams
			{
				 .m_Filter = Filter
				 , .m_nMaxToDelete = nMaxToDelete
				 , .m_bPretend = bPretend
			}
		;

		for (auto &TrustedActor : mp_DebugManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			auto DeleteResult = co_await TrustedActor.m_Actor.f_CallActor(&CDebugManager::f_Asset_Delete)(DeleteParams);

			*_pCommandLine += "{}: {} {} assets, {ns } files and {ns } bytes\n"_f
				<< TrustedActor.m_TrustInfo.m_HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
				<< (bPretend ? "Would have deleted" : "Deleted")
				<< DeleteResult.m_nAssetsDeleted
				<< DeleteResult.m_nFilesDeleted
				<< DeleteResult.m_nBytesDeleted
			;
		}

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_DebugManager_CrashDumpList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Host = _Params["DebugManagerHost"].f_String();
		bool bIncludeHost = _Params["IncludeHost"].f_Boolean();
		bool bIncludeExceptionInfo = _Params["IncludeExceptionInfo"].f_Boolean();

		co_await fp_DebugManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for debug managers");

		auto Filter = fg_ParseCrashDumpFilter(_Params);

		TCFutureMap<CHostInfo, CDebugManager::CCrashDumpList::CResult> ListResults;
		for (auto &TrustedActor : mp_DebugManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;
			TrustedActor.m_Actor.f_CallActor(&CDebugManager::f_CrashDump_List)(CDebugManager::CCrashDumpList{.m_Filter = Filter})
				.f_Timeout(mp_Timeout, "Timed out waiting for debug manager to reply")
				> ListResults[TrustedActor.m_TrustInfo.m_HostInfo]
			;
		}

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();

		CTableRenderHelper::CColumnHelper Columns(_Params["Verbosity"].f_Integer());

		Columns.f_AddHeading("Host", bIncludeHost ? 2 : 10);
		Columns.f_AddHeading("ID", 0);
		Columns.f_AddHeading("File Name", 1);
		Columns.f_AddHeading("Timestamp", 3);
		Columns.f_AddHeading("Digest", 3);
		Columns.f_AddHeading("Size", 3);
		Columns.f_AddHeading("Compressed", 4);
		Columns.f_AddHeading("Ratio", 4);
		Columns.f_AddHeading("Product", 6);
		Columns.f_AddHeading("Application", 2);
		Columns.f_AddHeading("Config", 5);
		Columns.f_AddHeading("Git Branch", 4);
		Columns.f_AddHeading("Git Commit", 7);
		Columns.f_AddHeading("Platform", 5);
		Columns.f_AddHeading("Version", 4);
		Columns.f_AddHeading("Tags", 4);
		Columns.f_AddHeading("Exception Info", bIncludeExceptionInfo ? 2 : 10);

		Columns.f_SetSortByColumns({"Timestamp"});

		TableRenderer.f_AddHeadings(&Columns);
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);

		auto Results = co_await fg_AllDoneWrapped(ListResults);
		for (auto &Result : Results)
		{
			auto &HostInfo = Results.fs_GetKey(Result);
			CStr HostDescription = HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags);
			if (!Result)
			{
				*_pCommandLine %= "{}Failed getting applications for host{} '{}': {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					<< Result.f_GetExceptionStr()
				;
				continue;
			}

			for (auto iCrashDumps = co_await fg_Move(Result->m_CrashDumpsGenerator).f_GetPipelinedIterator(); iCrashDumps; co_await ++iCrashDumps)
			{
				for (auto &CrashDump : *iCrashDumps)
				{
					TableRenderer.f_AddRow
						(
							HostDescription
							, CrashDump.m_ID
							, CrashDump.m_FileInfo.m_FileName
							, CrashDump.m_FileInfo.m_Timestamp
							, CrashDump.m_FileInfo.m_Digest.f_GetString()
							, fg_FormatBytes(CrashDump.m_FileInfo.m_Size)
							, fg_FormatBytes(CrashDump.m_FileInfo.m_CompressedSize)
							, "{fe1}"_f << (fp64(CrashDump.m_FileInfo.m_Size) / fp64(CrashDump.m_FileInfo.m_CompressedSize))
							, CrashDump.m_Metadata.m_Product ? *CrashDump.m_Metadata.m_Product : CStr()
							, CrashDump.m_Metadata.m_Application ? *CrashDump.m_Metadata.m_Application : CStr()
							, CrashDump.m_Metadata.m_Configuration ? *CrashDump.m_Metadata.m_Configuration : CStr()
							, CrashDump.m_Metadata.m_GitBranch ? *CrashDump.m_Metadata.m_GitBranch : CStr()
							, CrashDump.m_Metadata.m_GitCommit ? *CrashDump.m_Metadata.m_GitCommit : CStr()
							, CrashDump.m_Metadata.m_Platform ? *CrashDump.m_Metadata.m_Platform : CStr()
							, CrashDump.m_Metadata.m_Version ? *CrashDump.m_Metadata.m_Version : CStr()
							, CrashDump.m_Metadata.m_Tags ? "{vs}"_f << *CrashDump.m_Metadata.m_Tags : CStr()
							, CrashDump.m_ExceptionInfo ? *CrashDump.m_ExceptionInfo : CStr()
						)
					;
				}
			}
		}

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_DebugManager_CrashDumpUpload(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto &CurrentDirectory = _Params["CurrentDirectory"].f_String();

		auto RelativeSources = _Params["Sources"].f_StringArray();
		if (RelativeSources.f_IsEmpty())
			co_return DMibErrorInstance("You must speciy at least one source");

		TCVector<CStr> Sources;
		for (auto &RelativeSource : RelativeSources)
		{
			auto Source = CFile::fs_GetExpandedPath(RelativeSource, CurrentDirectory);
			if (Source.f_IsEmpty())
				co_return DMibErrorInstance("Sources must not be empty");

			Sources.f_Insert(fg_Move(Source));
		}

		CStr Host = _Params["DebugManagerHost"].f_String();
		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		int32 CompressionLevel = _Params["CompressionLevel"].f_Integer();
		bool bForce = _Params["Force"].f_Boolean();
		auto Metadata = fg_ParseMetadataOptions(_Params);
		TCOptional<CStr> ExceptionInfo;
		if (auto *pValue = _Params.f_GetMember("ExceptionInfo"))
			ExceptionInfo = pValue->f_String();

		TCMap<CStr, CDebugManager::CCrashDumpInfos::CUniqueCrashDump> ToUpload;

		CStr CrashDumpID;
		if (auto pValue = _Params.f_GetMember("ID"))
		{
			CrashDumpID = pValue->f_String();
			auto &CrashDumpUpload = ToUpload[CrashDumpID];
			CrashDumpUpload.m_Sources.f_Insert(Sources);
			CrashDumpUpload.m_Metadata = Metadata;
			CrashDumpUpload.m_ExceptionInfo = ExceptionInfo;
		}
		else
		{
			auto GatherResult = co_await CDebugManager::fs_GatherCrashDumpInfos(Sources, Metadata, ExceptionInfo);

			for (auto &ErrorEntry : GatherResult.m_Errors.f_Entries())
				*_pCommandLine %= "{}: {}\n"_f << ErrorEntry.f_Key() << ErrorEntry.f_Value();

			ToUpload = fg_Move(GatherResult.m_Uploads);

			if (ToUpload.f_IsEmpty())
				co_return DMibErrorInstance("Nothing found to upload");
		}

		if (QueueSize < 128 * 1024)
			QueueSize = 128 * 1024;

		auto FormatFlags = _pCommandLine->f_AnsiEncoding().f_Flags();

		co_await fp_DebugManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for debug managers");

		auto DebugManagers = co_await fp_DebugManager_GetDebugManagers(Host);

		TCMap<CHostInfo, TCFutureVector<CDebugManagerHelper::CUploadResult>> Results;

		for (auto &DebugManager : DebugManagers)
		{
			for (auto &Upload : ToUpload.f_Entries())
			{
				for (auto &Source : Upload.f_Value().m_Sources)
				{
					mp_DebugManagerHelper.f_CrashDump_Upload
						(
							DebugManager.m_Actor
							, Source
							, Upload.f_Key()
							, Upload.f_Value().m_Metadata
							, Upload.f_Value().m_ExceptionInfo
							, bForce ? CDebugManager::EUploadFlag::mc_ForceOverwrite : CDebugManager::EUploadFlag::mc_None
							, QueueSize
							, CompressionLevel
						)
						> Results[DebugManager.m_TrustInfo.m_HostInfo]
					;
				}
			}
		}

		for (auto &ResultEntry : Results.f_Entries())
		{
			auto &HostInfo = ResultEntry.f_Key();

			for (auto &Result : co_await fg_AllDoneWrapped(ResultEntry.f_Value()))
			{
				if (!Result)
				{
					*_pCommandLine %= "Failed to upload to '{}': {}\n"_f << HostInfo.f_GetDescColored(FormatFlags) << Result.f_GetExceptionStr();
					continue;
				}

				auto &UploadResult = *Result;

				*_pCommandLine += "Upload finished transferring to '{}': {ns } bytes at {fe2} MB/s\n"_f
					<< HostInfo.f_GetDescColored(FormatFlags)
					<< UploadResult.m_TransferResult.m_nBytes
					<< UploadResult.m_TransferResult.f_BytesPerSecond() / 1'000'000.0
				;
			}
		}

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_DebugManager_CrashDumpDownload(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Host = _Params["DebugManagerHost"].f_String();
		CStr DestinationDirectory = CFile::fs_GetExpandedPath(_Params["DestinationDirectory"].f_String(), _Params["CurrentDirectory"].f_String());
		if (DestinationDirectory.f_IsEmpty())
			co_return DMibErrorInstance("Destination directory must be specified");

		uint64 QueueSize = _Params["TransferQueueSize"].f_Integer();
		bool bAllowMultiDownload = _Params["AllowMultiDownload"].f_Boolean();
		auto Filter = fg_ParseCrashDumpFilter(_Params);

		if (!bAllowMultiDownload && !Filter.m_ID)
			co_return DMibErrorInstance("Your filtering options could match multiple crash dump IDs. If you want to allow this use the -f flag.");

		TCOptional<CStr> FileName;
		if (auto pValue = _Params.f_GetMember("FileName"))
			FileName = pValue->f_String();

		if (QueueSize < 128 * 1024)
			QueueSize = 128 * 1024;

		auto FormatFlags = _pCommandLine->f_AnsiEncoding().f_Flags();

		co_await fp_DebugManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for debug managers");

		auto DebugManagers = co_await fp_DebugManager_GetDebugManagers(Host);

		CExceptionExceptionVectorData::CErrorCollector Collector;

		for (auto &DebugManager : DebugManagers)
		{
			auto Results = co_await mp_DebugManagerHelper.f_CrashDump_Download
				(
					DebugManager.m_Actor
					, Filter
					, DestinationDirectory
					, CFileTransferReceive::EReceiveFlag_None
					, QueueSize
				)
				.f_Wrap()
			;

			if (!Results)
			{
				Collector.f_AddError
					(
						DMibErrorInstanceWrapped
						(
							"Failed to download from '{}': {}"_f << DebugManager.m_TrustInfo.m_HostInfo.f_GetDescColored(FormatFlags) << Results.f_GetExceptionStr()
							, Results.f_ExceptionPointer()
						)
					)
				;
				continue;
			}

			*_pCommandLine += "Download finished transferring from '{}': {ns } bytes at {fe2} MB/s\n"_f
				<< DebugManager.m_TrustInfo.m_HostInfo.f_GetDescColored(FormatFlags)
				<< Results->m_nBytes
				<< Results->f_BytesPerSecond() / 1'000'000.0
			;

			co_return 0;
		}

		if (Collector.f_HasError())
			co_return fg_Move(Collector).f_GetException();

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_DebugManager_CrashDumpDelete(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Host = _Params["DebugManagerHost"].f_String();
		uint64 nMaxToDelete = _Params["MaxDeletes"].f_Integer();
		bool bAllowMultiDelete = _Params["AllowMultiDelete"].f_Boolean();
		bool bPretend = _Params["Pretend"].f_Boolean();
		auto Filter = fg_ParseCrashDumpFilter(_Params);

		if (!bAllowMultiDelete && !Filter.m_ID)
			co_return DMibErrorInstance("Your filtering options could match multiple crash dump IDs. If you want to allow this use the -f flag.");

		co_await fp_DebugManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for debug managers");

		CDebugManager::CCrashDumpDelete DeleteParams
			{
				 .m_Filter = Filter
				 , .m_nMaxToDelete = nMaxToDelete
				 , .m_bPretend = bPretend
			}
		;

		for (auto &TrustedActor : mp_DebugManagers.m_Actors)
		{
			if (!Host.f_IsEmpty() && TrustedActor.m_TrustInfo.m_HostInfo.m_HostID != Host)
				continue;

			auto DeleteResult = co_await TrustedActor.m_Actor.f_CallActor(&CDebugManager::f_CrashDump_Delete)(DeleteParams);

			*_pCommandLine += "{}: {} {} crash dumps, {ns } files and {ns } bytes\n"_f
				<< TrustedActor.m_TrustInfo.m_HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
				<< (bPretend ? "Would have deleted" : "Deleted")
				<< DeleteResult.m_nCrashDumpsDeleted
				<< DeleteResult.m_nFilesDeleted
				<< DeleteResult.m_nBytesDeleted
			;
		}

		co_return 0;
	}
}
