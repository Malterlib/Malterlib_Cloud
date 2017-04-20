// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"

namespace NMib::NCloud
{
	auto CBackupManagerClient::CInternal::f_UpdateManifest(CStr const &_FileName, CStr const &_OriginalFileName) -> TCContinuation<CUpdateManifestResult>
	{
		if (_FileName.f_IsEmpty())
			return fg_Explicit(CUpdateManifestResult{});
			
		TCContinuation<CUpdateManifestResult> Continuation;
		
		g_Dispatch(m_FileActor) > [_FileName, _OriginalFileName, Config = m_Config]() mutable -> CUpdateManifestResult
			{
				auto AbsoluteFileName = CFile::fs_AppendPath(Config.m_ManifestConfig.m_Root, _OriginalFileName);
				
				if (!CFile::fs_FileExists(AbsoluteFileName, EFileAttrib_File | EFileAttrib_Link | EFileAttrib_Directory))
					return {{}, {}, false};
				
				CDirectoryManifestFile ManifestFile;
				ManifestFile.m_Attributes = CFile::fs_GetAttributes(AbsoluteFileName);
				
				CDirectoryManifest::fs_UpdateManifestFile(Config.m_ManifestConfig, _FileName, ManifestFile, _OriginalFileName);
				
				CUniqueFileIdentifier FileID;
				if (ManifestFile.m_Attributes & EFileAttrib_Link)
					FileID = CFile::fs_GetUniqueIdentifierOnLink(AbsoluteFileName);
				else
					FileID = CFile::fs_GetUniqueIdentifier(AbsoluteFileName);

				CUpdateManifestResult Result = {fg_Move(ManifestFile), {}, FileID, true};
				
				CStr Directory = CFile::fs_GetPath(_FileName);
				while (!Directory.f_IsEmpty())
				{
					auto &UpdatedDirectory = Result.m_UpdatedDirectories[Directory];

					CStr OriginalFileName;
					if (_FileName != _OriginalFileName)
					{
						if (ManifestFile.f_IsDirectory())
							OriginalFileName = _OriginalFileName;
						else
							OriginalFileName = CFile::fs_GetPath(_OriginalFileName);
					}
					else
						OriginalFileName = Directory;

					UpdatedDirectory.m_ManifestFile.m_Attributes = CFile::fs_GetAttributes(CFile::fs_AppendPath(Config.m_ManifestConfig.m_Root, OriginalFileName));
						
					CDirectoryManifest::fs_UpdateManifestFile(Config.m_ManifestConfig, Directory, UpdatedDirectory.m_ManifestFile, OriginalFileName);
					Directory = CFile::fs_GetPath(Directory);
				}
				
				return Result;
			}
			> Continuation / [this, _FileName, Continuation](CUpdateManifestResult &&_Result)
			{
				if (!_Result.m_bExists)
				{
					if (m_Manifest.m_Files.f_Remove(_FileName))
						_Result.m_bRemoved = true;
					if (m_ManifestFileIDs.f_Remove(_FileName))
						_Result.m_bIDChanged = true;
				}
				else
				{
					auto Mapped = m_Manifest.m_Files(_FileName);
					if (Mapped.f_WasCreated())
						_Result.m_bAdded = true;
						
					if (_Result.m_ManifestFile.f_IsFile())
					{
						auto FileID = m_ManifestFileIDs[_FileName];
						if (_Result.m_FileID != FileID)
						{
							FileID = _Result.m_FileID;
							_Result.m_bIDChanged = true;
						}
					}
					else
					{
						if (m_ManifestFileIDs.f_Remove(_FileName))
							_Result.m_bIDChanged = true;
					}

					*Mapped = _Result.m_ManifestFile;

					for (auto iDirectory = _Result.m_UpdatedDirectories.f_GetIterator(); iDirectory;)
					{
						auto &Directory = *iDirectory;
						auto Mapped = m_Manifest.m_Files(iDirectory.f_GetKey());
						auto &ManifestFile = *Mapped;
						
						if (Mapped.f_WasCreated())
						{
							ManifestFile = Directory.m_ManifestFile;
							Directory.m_bAdded = true;
							++iDirectory;
							continue;
						}
						
						if (ManifestFile != Directory.m_ManifestFile)
						{
							ManifestFile = Directory.m_ManifestFile;
							++iDirectory;
							continue;
						}
						
						iDirectory.f_Remove();
					}
				}
				
				Continuation.f_SetResult(fg_Move(_Result));
			}
		;
		
		return Continuation;
	}
}
