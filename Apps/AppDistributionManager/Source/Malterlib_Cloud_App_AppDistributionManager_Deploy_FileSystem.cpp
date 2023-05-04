// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppDistributionManager.h"

#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Process/ProcessLaunch>
#include <Mib/File/File>

namespace NMib::NCloud::NAppDistributionManager
{
	CDeployDestination_FileSystem::CDeployDestination_FileSystem()
	{
		mp_FileActor = fg_Construct(fg_Construct(), "DeployDestination_FileSystem file actor");
	}

	TCFuture<void> CDeployDestination_FileSystem::fp_Destroy()
	{
		CLogError LogError("Mib/Cloud/AppDistributionManager");

		co_await mp_FileActor.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy file actor in file system");

		co_return {};
	}

	namespace
	{
		struct CParsedVersion
		{
			auto operator <=> (CParsedVersion const &_Right) const = default;
			
			static CParsedVersion fs_Parse(CStr const &_String)
			{
				CParsedVersion Return;
				(CStr::CParse("{}.{}.{}") >> Return.m_Major >> Return.m_Minor >> Return.m_Revision).f_Parse(_String);
				return Return;
			}

			uint32 m_Major = 0;
			uint32 m_Minor = 0;
			uint32 m_Revision = 0;
		};

		CByteVector fg_GetDigest(CStr const &_FileName)
		{
			auto Digest = CFile::fs_GetFileChecksum_SHA512(_FileName);
			return CByteVector(Digest.f_GetData(), Digest.mc_Size);
		}

		CStr fg_EncodeElectronTime(CTime const &_Time)
		{
			CTimeConvert::CDateTime DateTime;
			CTimeConvert(_Time).f_ExtractDateTime(DateTime);

			return "{}-{sj2,sf0}-{sj2,sf0}T{sj2,sf0}:{sj2,sf0}:{sj2,sf0}.{sj3,sf0}Z"_f
				<< DateTime.m_Year
				<< DateTime.m_Month
				<< DateTime.m_DayOfMonth
				<< DateTime.m_Hour
				<< DateTime.m_Minute
				<< DateTime.m_Second
				<< (DateTime.m_Fraction * 1000.0).f_ToInt()
			;
		}

	}

	TCFuture<void> CDeployDestination_FileSystem::f_Deploy(CDeployInfo const &_DeployInfo)
	{
		co_await
			(
				g_Dispatch(mp_FileActor) / [=]() -> void
				{
					CStr TempDirectory = CFile::fs_GetProgramDirectory() / "Temp" / fg_RandomID();
					CStr TempFile = TempDirectory / "TempFile";

					CFile::fs_CreateDirectory(TempDirectory);

					auto Cleanup = g_OnScopeExit / [&]
						{
							try
							{
								if (CFile::fs_FileExists(TempDirectory))
									CFile::fs_DeleteDirectoryRecursive(TempDirectory);
							}
							catch (CExceptionFile const &)
							{
							}
						}
					;

					CStr DownloadFile;

					CEJSON Files = EJSONType_Object;

					CStr DestinationDirectory;

					if (_DeployInfo.m_bElectron)
					{
						DestinationDirectory = CFile::fs_GetProgramDirectory() / "Distribution" / _DeployInfo.m_Renamed;
						CFile::fs_CreateDirectory(DestinationDirectory);

						CProcessLaunchParams LaunchParams;
						LaunchParams.m_WorkingDirectory = TempDirectory;
						CProcessLaunch::fs_LaunchTool
							(
								"tar"
								,
								{
									"--no-same-owner"
	#if !defined(DPlatformFamily_macOS)
									, "--pax-option=delete=SCHILY.*,delete=LIBARCHIVE.*"
	#endif
									, "-xf"
									, _DeployInfo.m_SourceFile

								}
								, LaunchParams
							)
						;
						auto DistributionFiles = CFile::fs_FindFiles(TempDirectory / "*");
						for (auto &File : DistributionFiles)
						{
							if (CFile::fs_GetFile(File).f_StartsWith("."))
								continue;

							CStr RelativePath = CFile::fs_MakePathRelative(File, TempDirectory);
							CStr Destination = DestinationDirectory / RelativePath;
							CStr RelativeDestination = _DeployInfo.m_Renamed / RelativePath;
							auto Extension = CFile::fs_GetExtension(Destination);

							auto &OutputFileInfo = Files[RelativeDestination];

							OutputFileInfo =
								{
									"Digest"_= fg_GetDigest(File)
									, "Size"_= CFile::fs_GetFileSize(File)
								}
							;

							if (Extension == "AppImage")
							{
								CFile ReadFile;
								ReadFile.f_Open(File, EFileOpen_Read | EFileOpen_ShareAll);
								uint32 Size = 0;
								ReadFile.f_SetPositionFromEnd(-4);
								ReadFile.f_Read(&Size, sizeof(Size));
								Size = fg_ByteSwapBE(Size);
								OutputFileInfo["BlockMapSize"] = Size;
							}

							if (CFile::fs_FileExists(Destination))
								CFile::fs_AtomicReplaceFile(File, Destination);
							else
								CFile::fs_RenameFile(File, Destination);

							if (Extension == "exe" || Extension == "dmg" || Extension == "AppImage")
								DownloadFile = RelativeDestination;
						}
					}
					else
					{
						CStr DestinationFile = CFile::fs_GetProgramDirectory() / "Distribution" / _DeployInfo.m_Renamed;
						DestinationDirectory = CFile::fs_GetPath(DestinationFile);
						CFile::fs_CreateDirectory(DestinationDirectory);

						Files[_DeployInfo.m_Renamed] =
							{
								"Digest"_= fg_GetDigest(_DeployInfo.m_SourceFile)
								, "Size"_= CFile::fs_GetFileSize(_DeployInfo.m_SourceFile)
							}
						;

						if (!CFile::fs_TryDuplicateFile(_DeployInfo.m_SourceFile, TempFile))
							CFile::fs_CopyFile(_DeployInfo.m_SourceFile, TempFile);

						if (CFile::fs_FileExists(DestinationFile))
							CFile::fs_AtomicReplaceFile(TempFile, DestinationFile);
						else
							CFile::fs_RenameFile(TempFile, DestinationFile);

						DownloadFile = _DeployInfo.m_Renamed;
					}

					CEJSON LatestRelease;
					{
						CStr DatabaseFile = DestinationDirectory / "Releases.json";
						CEJSON Database;
						if (CFile::fs_FileExists(DatabaseFile))
							Database = CEJSON::fs_FromString(CFile::fs_ReadStringFromFile(DatabaseFile, true), DatabaseFile);

						auto &Releases = Database["Releases"];

						CStr ReleaseNotes;

						if (auto pAppDistribution = _DeployInfo.m_Version.m_VersionInfo.m_ExtraInfo.f_GetMember("AppDistribution", EJSONType_Object))
						{
							if (auto pReleaseNotes = pAppDistribution->f_GetMember("ReleaseNotes", EJSONType_String))
								ReleaseNotes = pReleaseNotes->f_String();
						}

						CStr VersionString = "{}.{}.{}"_f
							<< _DeployInfo.m_Version.m_VersionID.m_VersionID.m_Major
							<< _DeployInfo.m_Version.m_VersionID.m_VersionID.m_Minor
							<< _DeployInfo.m_Version.m_VersionID.m_VersionID.m_Revision
						;

						CEJSON NewRelease =
							{
								"Path"_= DownloadFile
								, "Version"_= _DeployInfo.m_Version.m_VersionID.f_ToJson()
								, "VersionString"_= VersionString
								, "Time"_= _DeployInfo.m_Version.m_VersionInfo.m_Time
								, "TimeUnixMilliSeconds"_= CTimeConvert(_DeployInfo.m_Version.m_VersionInfo.m_Time).f_UnixMilliseconds()
								, "TimeString"_= "{}"_f << _DeployInfo.m_Version.m_VersionInfo.m_Time
								, "ReleaseNotes"_= "{}"_f << ReleaseNotes
								, "Files"_= fg_TempCopy(Files)
							}
						;

						bool bFoundRelease = false;
						for (auto &Release : Releases.f_Array())
						{
							if (Release["Version"] == NewRelease["Version"])
							{
								bFoundRelease = true;
								Release = NewRelease;
								break;
							}
						}

						if (!bFoundRelease)
							Releases.f_Array().f_InsertFirst(NewRelease);

						Releases.f_Array().f_Sort
							(
								[](CEJSON const &_Left, CEJSON const &_Right)
								{
									return CParsedVersion::fs_Parse(_Right["VersionString"].f_String()) <=> CParsedVersion::fs_Parse(_Left["VersionString"].f_String());
								}
							)
						;

						LatestRelease = Releases.f_Array()[0];
						Database["LatestRelease"] = LatestRelease;

						CFile::fs_WriteStringToFile(TempFile, Database.f_ToString(), false);

						if (CFile::fs_FileExists(DatabaseFile))
							CFile::fs_AtomicReplaceFile(TempFile, DatabaseFile);
						else
							CFile::fs_RenameFile(TempFile, DatabaseFile);

						{
							CStr LatestReleaseFile = DestinationDirectory / "Latest.json";
							CFile::fs_WriteStringToFile(TempFile, LatestRelease.f_ToString(), false);

							if (CFile::fs_FileExists(LatestReleaseFile))
								CFile::fs_AtomicReplaceFile(TempFile, LatestReleaseFile);
							else
								CFile::fs_RenameFile(TempFile, LatestReleaseFile);
						}
					}

					{
						CStr LatestHTMLRedirect = "<meta HTTP-EQUIV=\"REFRESH\" content=\"0; url={}\">\n"_f << CFile::fs_GetFile(LatestRelease["Path"].f_String());
						CStr LatestHTMLRedirectFile = DestinationDirectory / "Latest.html";
						CFile::fs_WriteStringToFile(TempFile, LatestHTMLRedirect, false);

						if (CFile::fs_FileExists(LatestHTMLRedirectFile))
							CFile::fs_AtomicReplaceFile(TempFile, LatestHTMLRedirectFile);
						else
							CFile::fs_RenameFile(TempFile, LatestHTMLRedirectFile);
					}

					if (_DeployInfo.m_bElectron)
					{
						CStr LatestContents = "version: {}\n"_f << LatestRelease["VersionString"].f_String();
						LatestContents += "files:\n";

						CStr ReleaseFileName;
						CStr ReleaseDigest;
						for (auto &File : LatestRelease["Files"].f_Object())
						{
							CStr FileName = File.f_Name();
							auto &FileValue = File.f_Value();
							CStr Extension = CFile::fs_GetExtension(FileName);
							auto Digest = fg_Base64Encode(FileValue["Digest"].f_Binary());

							if (Extension == "blockmap")
								continue;

							LatestContents += "  - url: {}\n"_f << CFile::fs_GetFile(FileName);
							LatestContents += "    sha512: {}\n"_f << Digest;
							LatestContents += "    size: {}\n"_f << FileValue["Size"].f_Integer();

							if (auto pValue = FileValue.f_GetMember("BlockMapSize"))
								LatestContents += "    blockMapSize: {}\n"_f << pValue->f_Integer();

							if (Extension == "exe" || Extension == "AppImage" || Extension == "dmg")
							{
								ReleaseFileName = FileName;
								ReleaseDigest = Digest;
							}
						}

						LatestContents += "path: {}\n"_f << ReleaseFileName;
						LatestContents += "sha512: {}\n"_f << ReleaseDigest;
						LatestContents += "releaseDate: '{}'\n"_f << fg_EncodeElectronTime(LatestRelease["Time"].f_Date());

						CStr LatestFileName;

						if (_DeployInfo.m_ElectronPlatform == "Windows")
							LatestFileName = DestinationDirectory / "latest.yml";
						else if (_DeployInfo.m_ElectronPlatform == "Linux")
							LatestFileName = DestinationDirectory / "latest-linux.yml";
						else if (_DeployInfo.m_ElectronPlatform == "macOS")
							LatestFileName = DestinationDirectory / "latest-mac.yml";
						else
							DMibError("Unknown electron platform: {}"_f << _DeployInfo.m_ElectronPlatform);

						CFile::fs_WriteStringToFile(TempFile, LatestContents, false);

						if (CFile::fs_FileExists(LatestFileName))
							CFile::fs_AtomicReplaceFile(TempFile, LatestFileName);
						else
							CFile::fs_RenameFile(TempFile, LatestFileName);
					}
				}
			)
		;

		co_return {};
	}
}
