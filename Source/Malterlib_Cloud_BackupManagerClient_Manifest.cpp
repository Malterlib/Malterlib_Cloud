// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"

namespace NMib::NCloud
{
	auto CBackupManagerClient::CInternal::f_UpdateManifest(CStr const &_FileName) -> TCContinuation<CUpdateManifestResult>
	{
		if (_FileName.f_IsEmpty())
			return fg_Explicit(CUpdateManifestResult{});
			
		TCContinuation<CUpdateManifestResult> Continuation;
		
		g_Dispatch(m_FileActor) > [_FileName, Config = m_Config]() mutable -> CUpdateManifestResult
			{
				auto AbsoluteFileName = CFile::fs_AppendPath(Config.m_ManifestConfig.m_Root, _FileName);
				
				if (!CFile::fs_FileExists(AbsoluteFileName, EFileAttrib_File | EFileAttrib_Link | EFileAttrib_Directory))
					return {{}, {}, false};
				
				CDirectoryManifestFile ManifestFile;
				ManifestFile.m_Attributes = CFile::fs_GetAttributes(AbsoluteFileName);
				
				CDirectoryManifest::fs_UpdateManifestFile(Config.m_ManifestConfig, _FileName, ManifestFile);
				
				CUpdateManifestResult Result = {fg_Move(ManifestFile), {}, true};
				
				CStr Directory = CFile::fs_GetPath(_FileName);
				while (!Directory.f_IsEmpty())
				{
					auto &UpdatedDirectory = Result.m_UpdatedDirectories[Directory];
					UpdatedDirectory.m_ManifestFile.m_Attributes = CFile::fs_GetAttributes(CFile::fs_AppendPath(Config.m_ManifestConfig.m_Root, Directory));
					CDirectoryManifest::fs_UpdateManifestFile(Config.m_ManifestConfig, Directory, UpdatedDirectory.m_ManifestFile);
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
				}
				else
				{
					auto Mapped = m_Manifest.m_Files(_FileName);
					if (Mapped.f_WasCreated())
						_Result.m_bAdded = true;
						
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
