// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppDistributionManager.h"
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JSONShortcuts>

namespace NMib::NCloud::NAppDistributionManager
{
	CDeployDestination_FileSystem::CDeployDestination_FileSystem()
	{
		mp_FileActor = fg_Construct(fg_Construct(), "DeployDestination_FileSystem file actor");
	}

	TCContinuation<void> CDeployDestination_FileSystem::fp_Destroy()
	{
		TCSharedPointer<CCanDestroyTracker> pDestroyTracker = fg_Construct();
		mp_FileActor->f_Destroy() > pDestroyTracker->f_Track();

		return pDestroyTracker->m_Continuation;
	}

	TCContinuation<void> CDeployDestination_FileSystem::f_Deploy(CStr const &_SourceFile, CApplicationVersion const &_Version, CDistributionSettings const &_Settings, CStr const &_Renamed)
	{
		return g_Dispatch(mp_FileActor) > [=]() -> void
			{
				CStr DestinationFile = CFile::fs_GetProgramDirectory() / "Distribution" / _Renamed;
				CStr DestinationDirectory = CFile::fs_GetPath(DestinationFile);
				CStr TempDirectory = CFile::fs_GetProgramDirectory() / "Temp" / fg_RandomID();
				CStr TempFile = TempDirectory / "TempFile";

				CFile::fs_CreateDirectory(DestinationDirectory);
				CFile::fs_CreateDirectory(TempDirectory);

				auto Cleanup = g_OnScopeExit > [&]
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

				{
					if (!CFile::fs_TryDuplicateFile(_SourceFile, TempFile))
						CFile::fs_CopyFile(_SourceFile, TempFile);

					if (CFile::fs_FileExists(DestinationFile))
						CFile::fs_AtomicReplaceFile(TempFile, DestinationFile);
					else
						CFile::fs_RenameFile(TempFile, DestinationFile);
				}

				{
					CStr DatabaseFile = DestinationDirectory / "Releases.json";
					CJSON Database;
					if (CFile::fs_FileExists(DatabaseFile))
						Database = CJSON::fs_FromString(CFile::fs_ReadStringFromFile(DatabaseFile, true), DatabaseFile);

					auto &Releases = Database["Releases"];

					CStr ReleaseNotes;

					if (auto pAppDistribution = _Version.m_VersionInfo.m_ExtraInfo.f_GetMember("AppDistribution", EJSONType_Object))
					{
						if (auto pReleaseNotes = pAppDistribution->f_GetMember("ReleaseNotes", EJSONType_String))
							ReleaseNotes = pReleaseNotes->f_String();
					}

					CStr VersionString = "{}.{}.{}"_f << _Version.m_VersionID.m_VersionID.m_Major << _Version.m_VersionID.m_VersionID.m_Minor << _Version.m_VersionID.m_VersionID.m_Revision;

					CJSON NewRelease =
						{
							"Path"__= _Renamed
							, "Version"__= _Version.m_VersionID.f_ToJSON().f_ToJSON()
							, "VersionString"__= VersionString
							, "Time"__= CTimeConvert(_Version.m_VersionInfo.m_Time).f_UnixMilliseconds()
							, "TimeString"__= "{}"_f << _Version.m_VersionInfo.m_Time
							, "ReleaseNotes"__= "{}"_f << ReleaseNotes
						}
					;

					Database["LatestRelease"] = NewRelease;

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

					CFile::fs_WriteStringToFile(TempFile, Database.f_ToString(), false);

					if (CFile::fs_FileExists(DatabaseFile))
						CFile::fs_AtomicReplaceFile(TempFile, DatabaseFile);
					else
						CFile::fs_RenameFile(TempFile, DatabaseFile);

					{
						CStr LatestReleaseFile = DestinationDirectory / "Latest.json";
						CFile::fs_WriteStringToFile(TempFile, NewRelease.f_ToString(), false);

						if (CFile::fs_FileExists(LatestReleaseFile))
							CFile::fs_AtomicReplaceFile(TempFile, LatestReleaseFile);
						else
							CFile::fs_RenameFile(TempFile, LatestReleaseFile);
					}
				}

				{
					CStr LatestHTMLRedirect = "<meta HTTP-EQUIV=\"REFRESH\" content=\"0; url={}\">\n"_f << CFile::fs_GetFile(_Renamed);
					CStr LatestHTMLRedirectFile = DestinationDirectory / "Latest.html";
					CFile::fs_WriteStringToFile(TempFile, LatestHTMLRedirect, false);

					if (CFile::fs_FileExists(LatestHTMLRedirectFile))
						CFile::fs_AtomicReplaceFile(TempFile, LatestHTMLRedirectFile);
					else
						CFile::fs_RenameFile(TempFile, LatestHTMLRedirectFile);
				}
			}
		;
	}
}
