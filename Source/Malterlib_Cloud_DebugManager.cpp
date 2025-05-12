// Copyright © 2025 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_DebugManager.h"

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NContainer;
	using namespace NEncoding;
	using namespace NException;
	using namespace NFile;
	using namespace NStorage;
	using namespace NStr;

	CDebugManager::CDebugManager()
	{
		DMibPublishActorFunction(CDebugManager::f_Asset_List);
		DMibPublishActorFunction(CDebugManager::f_Asset_Upload);
		DMibPublishActorFunction(CDebugManager::f_Asset_Download);
		DMibPublishActorFunction(CDebugManager::f_Asset_Delete);

		DMibPublishActorFunction(CDebugManager::f_CrashDump_List);
		DMibPublishActorFunction(CDebugManager::f_CrashDump_Upload);
		DMibPublishActorFunction(CDebugManager::f_CrashDump_Download);
		DMibPublishActorFunction(CDebugManager::f_CrashDump_Delete);
	}

	CDebugManager::~CDebugManager() = default;

	CStr CDebugManager::fs_AssetTypeToStr(EAssetType _AssetType)
	{
		switch (_AssetType)
		{
		case EAssetType::mc_Executable: return gc_Str<"Executable">;
		case EAssetType::mc_DebugInfo: return gc_Str<"DebugInfo">;
		}
		return {};
	}

	auto CDebugManager::fs_AssetTypeFromStr(CStr const &_String) -> EAssetType
	{
		if (_String == gc_Str<"Executable">.m_Str)
			return EAssetType::mc_Executable;
		else if (_String == gc_Str<"DebugInfo">.m_Str)
			return EAssetType::mc_DebugInfo;

		DMibNeverGetHere;

		return EAssetType::mc_Executable;
	}

	auto CDebugManager::fs_GatherCrashDumpInfos
	(
		TCVector<CStr> _Sources
		, CMetadata _DefaultMetadata
		, TCOptional<CStr> _DefaultExceptionInfo
	)
	-> TCFuture<CCrashDumpInfos>
	{
		auto BlockingActorCheckout = fg_BlockingActor();
		co_return co_await
			(
				g_Dispatch(BlockingActorCheckout)
				/ [Metadata = _DefaultMetadata, Sources = _Sources, ExceptionInfo = _DefaultExceptionInfo]()
				-> TCFuture<CCrashDumpInfos>
				{
					auto CaptureExceptions = co_await (g_CaptureExceptions % "Failed to gather crash dump info");

					struct CMetadataCache
					{
						CMetadata m_Metadata;
						TCOptional<CStr> m_ExceptionInfo;
					};

					TCMap<CStr, CMetadataCache> MetadataCache;

					CCrashDumpInfos Result;

					auto fAddFile = [&](CStr const &_Path)
						{
							auto FileName = CFile::fs_GetFile(_Path);

							auto fParseWindows = [&](CStr const &_Prefix, CStr const &_Extension) -> CStr
								{
									if (!FileName.f_StartsWithNoCase(_Prefix))
										return {};

									auto *pParse = FileName.f_GetStr() + _Prefix.f_GetLen();
									auto *pIdStart = pParse;

									auto fParseNumber = [&](ch8 _Delimiter) -> bool
										{
											if (!fg_CharIsNumber(*pParse))
												return false;

											fg_ParseNumeric(pParse);

											if (*pParse != _Delimiter)
												return false;
											++pParse;

											return true;
										}
									;

									// Year
									if (!fParseNumber('-'))
										return {};

									// Month
									if (!fParseNumber('-'))
										return {};

									// Day
									if (!fParseNumber('_'))
										return {};

									// Hour
									if (!fParseNumber('.'))
										return {};

									// Minute
									if (!fParseNumber('.'))
										return {};

									// Second
									if (!fParseNumber('.'))
										return {};

									// Millisecond
									if (!fParseNumber('.'))
										return {};

									if (!fg_CharIsAlphabetical(*pParse) && !fg_CharIsNumber(*pParse))
										return {};

									fg_ParseAlphaNumeric(pParse);
									auto *pIdEnd = pParse;

									if (*pParse != '.')
										return {};
									++pParse;

									if (fg_StrCmpNoCase(pParse, _Extension) != 0)
										return {};

									return CStr(CInitByRange(), pIdStart, pIdEnd);
								}
							;

							auto fParseBreakpad = [&](CStr const &_Extension) -> CStr
								{
									auto *pParse = FileName.f_GetStr();
									auto *pIdStart = pParse;

									auto fParseHexNumber = [&](ch8 _Delimiter = 0) -> bool
										{
											if (!fg_CharIsHexNumber(*pParse))
												return false;

											while (*pParse && fg_CharIsHexNumber(*pParse))
												++pParse;

											if (_Delimiter)
											{
												if (*pParse != _Delimiter)
													return false;
												++pParse;
											}

											return true;
										}
									;

									// Guid0
									if (!fParseHexNumber('-'))
										return {};

									// Guid1
									if (!fParseHexNumber('-'))
										return {};

									// Guid2
									if (!fParseHexNumber('-'))
										return {};

									// Guid3
									if (!fParseHexNumber('-'))
										return {};

									// Guid4
									if (!fParseHexNumber())
										return {};

									auto *pIdEnd = pParse;

									if (*pParse != '.')
										return {};
									++pParse;

									if (fg_StrCmpNoCase(pParse, _Extension) != 0)
										return {};

									return CStr(CInitByRange(), pIdStart, pIdEnd);
								}
							;

							CStr CrashDumpID;
							CStr MetadataPath;
							if (auto Value = fParseBreakpad(gc_Str<"json">))
								return;
							else if (auto Value = fParseWindows(gc_Str<"Metadata_">, gc_Str<"json">))
								return;
							else if (auto Value = fParseBreakpad(gc_Str<"dmp">))
							{
								CrashDumpID = Value;
								MetadataPath = CFile::fs_GetPath(_Path) / ("{}.json"_f << CrashDumpID);
							}
							else if (auto Value = fParseWindows(gc_Str<"CrashLog_">, gc_Str<"txt">))
							{
								CrashDumpID = Value;
								MetadataPath = CFile::fs_GetPath(_Path) / ("Metadata_{}.json"_f << CrashDumpID);
							}
							else if (auto Value = fParseWindows(gc_Str<"FullDump_">, gc_Str<"dmp">))
							{
								CrashDumpID = Value;
								MetadataPath = CFile::fs_GetPath(_Path) / ("Metadata_{}.json"_f << CrashDumpID);
							}
							else if (auto Value = fParseWindows(gc_Str<"MiniDump_">, gc_Str<"dmp">))
							{
								CrashDumpID = Value;
								MetadataPath = CFile::fs_GetPath(_Path) / ("Metadata_{}.json"_f << CrashDumpID);
							}
							else
							{
								Result.m_Errors[_Path] = "Could not determine crash dump ID from path.";
								return;
							}

							auto MetadataCacheMapResult = MetadataCache(MetadataPath);
							auto &MetadataCache = *MetadataCacheMapResult;
							if (MetadataCacheMapResult.f_WasCreated())
							{
								MetadataCache.m_Metadata = Metadata;
								MetadataCache.m_ExceptionInfo = ExceptionInfo;

								try
								{
									if (CFile::fs_FileExists(MetadataPath))
									{
										auto JsonString = CFile::fs_ReadStringFromFile(MetadataPath, true);

										auto MetadataJson = CEJsonSorted::fs_FromString(JsonString, MetadataPath);

										if (auto *pValue = MetadataJson.f_GetMember("Product", EJsonType_String); pValue && !MetadataCache.m_Metadata.m_Product)
											MetadataCache.m_Metadata.m_Product = pValue->f_String();

										if (auto *pValue = MetadataJson.f_GetMember("Application", EJsonType_String); pValue && !MetadataCache.m_Metadata.m_Application)
											MetadataCache.m_Metadata.m_Application = pValue->f_String();

										if (auto *pValue = MetadataJson.f_GetMember("Configuration", EJsonType_String); pValue && !MetadataCache.m_Metadata.m_Configuration)
											MetadataCache.m_Metadata.m_Configuration = pValue->f_String();

										if (auto *pValue = MetadataJson.f_GetMember("GitBranch", EJsonType_String); pValue && !MetadataCache.m_Metadata.m_GitBranch)
											MetadataCache.m_Metadata.m_GitBranch = pValue->f_String();

										if (auto *pValue = MetadataJson.f_GetMember("GitCommit", EJsonType_String); pValue && !MetadataCache.m_Metadata.m_GitCommit)
											MetadataCache.m_Metadata.m_GitCommit = pValue->f_String();

										if (auto *pValue = MetadataJson.f_GetMember("Platform", EJsonType_String); pValue && !MetadataCache.m_Metadata.m_Platform)
											MetadataCache.m_Metadata.m_Platform = pValue->f_String();

										if (auto *pValue = MetadataJson.f_GetMember("Version", EJsonType_String); pValue && !MetadataCache.m_Metadata.m_Version)
											MetadataCache.m_Metadata.m_Version = pValue->f_String();

										if (auto *pValue = MetadataJson.f_GetMember("Tags", EJsonType_Array); pValue && pValue->f_IsStringArray() && !MetadataCache.m_Metadata.m_Tags)
											MetadataCache.m_Metadata.m_Tags = TCSet<CStr>::fs_FromContainer(pValue->f_StringArray());

										if (auto *pValue = MetadataJson.f_GetMember("ExceptionInfo", EJsonType_String); pValue && !MetadataCache.m_ExceptionInfo)
											MetadataCache.m_ExceptionInfo = pValue->f_String();
									}
									else
										Result.m_Errors[_Path] = "No metadata file found at: {}"_f << MetadataPath;
								}
								catch (CException const &_Exception)
								{
									Result.m_Errors[_Path] = "Error extracting metadata: {}"_f << _Exception;
									return;
								}
							}

							auto &Upload = Result.m_Uploads[CrashDumpID];
							Upload.m_Metadata = MetadataCache.m_Metadata;
							Upload.m_ExceptionInfo = MetadataCache.m_ExceptionInfo;
							Upload.m_Sources.f_Insert(_Path);
						}
					;

					for (auto &Source : Sources)
					{
						if (CFile::fs_FileExists(Source, EFileAttrib_Directory))
						{
							auto Files = CFile::fs_FindFiles(Source / "*", EFileAttrib_File, true);
							for (auto &File : Files)
								fAddFile(File);
						}
						else
							fAddFile(Source);
					}

					co_return fg_Move(Result);
				}
			)
		;
	}
}
