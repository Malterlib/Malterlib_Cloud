// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"

namespace NMib::NCloud
{
	void CBackupManagerClient::CInternal::fs_GetManifestFileProperties(CConfig const &_Config, CStr const &_FileName, CBackupManagerBackup::CManifestFile &o_ManifestFile)
	{
		auto FileName = CFile::fs_AppendPath(_Config.m_Root, _FileName);
		
		if (o_ManifestFile.m_Attributes & EFileAttrib_Link)
		{
			o_ManifestFile.m_SymlinkData = CFile::fs_ResolveSymbolicLink(FileName);
			o_ManifestFile.m_Owner = CFile::fs_GetOwnerOnLink(FileName);
			o_ManifestFile.m_Group = CFile::fs_GetGroupOnLink(FileName);
		}
		else if (o_ManifestFile.m_Attributes & EFileAttrib_Directory)
		{
			o_ManifestFile.m_Owner = CFile::fs_GetOwnerOnLink(FileName);
			o_ManifestFile.m_Group = CFile::fs_GetGroupOnLink(FileName);
			o_ManifestFile.m_WriteTime = CFile::fs_GetWriteTime(FileName);
		}
		else
		{
			CMibFilePos Length;
			o_ManifestFile.m_Digest = CFile::fs_GetFileChecksum_SHA256(FileName, &Length);
			o_ManifestFile.m_Length = Length;
			o_ManifestFile.m_WriteTime = CFile::fs_GetWriteTime(FileName);
			o_ManifestFile.m_Owner = CFile::fs_GetOwner(FileName);
			o_ManifestFile.m_Group = CFile::fs_GetGroup(FileName);
		}
		
		for (auto &Flags : _Config.m_AddSyncFlagsWildcards)
		{
			auto &Wildcard = _Config.m_AddSyncFlagsWildcards.fs_GetKey(Flags);
			if (fg_StrMatchWildcard(_FileName.f_GetStr(), Wildcard.f_GetStr()) == EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
				o_ManifestFile.m_Flags |= Flags;
		}

		for (auto &Flags : _Config.m_RemoveSyncFlagsWildcards)
		{
			auto &Wildcard = _Config.m_RemoveSyncFlagsWildcards.fs_GetKey(Flags);
			if (fg_StrMatchWildcard(_FileName.f_GetStr(), Wildcard.f_GetStr()) == EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
				o_ManifestFile.m_Flags &= ~Flags;
		}
	}
	
	CBackupManagerBackup::CManifest CBackupManagerClient::CInternal::fs_GetManifest(CConfig const &_Config)
	{
		CBackupManagerBackup::CManifest BackupManifest;
		
		for (auto &Wildcard : _Config.m_IncludeWildcards)
		{
			bool bRecursive = false;
			auto WildcardParsed = fs_ParseWildcard(Wildcard, bRecursive);
			
			auto FileTypes = EFileAttrib_File | EFileAttrib_Directory | EFileAttrib_Link;
			
			for (auto &FoundFile : CFile::fs_FindFilesEx(CFile::fs_AppendPath(_Config.m_Root, WildcardParsed), FileTypes, bRecursive, false))
			{
				CStr RelativePath = CFile::fs_MakePathRelative(FoundFile.m_Path, _Config.m_Root);
				auto Mapping = BackupManifest.m_Files(RelativePath);
				if (!Mapping.f_WasCreated())
					continue;
				
				auto &ManifestFile = *Mapping;
				ManifestFile.m_Attributes = FoundFile.m_Attribs;
			}
		}
		
		TCSet<CStr> ToRemove;
		TCSet<CStr> ImplicitDirectories;

		for (auto &ManifestFile : BackupManifest.m_Files)
		{
			auto &RelativePath = BackupManifest.m_Files.fs_GetKey(ManifestFile);
			if (fs_MatchesAnyWildcard(RelativePath, _Config.m_ExcludeWildcards))
			{
				ToRemove[RelativePath];
				continue;
			}
			
			CStr Directory = CFile::fs_GetPath(RelativePath);
			if (!Directory.f_IsEmpty())
				ImplicitDirectories[Directory];
			
			fs_GetManifestFileProperties(_Config, RelativePath, ManifestFile);
		}
		
		for (auto &File : ToRemove)
			BackupManifest.m_Files.f_Remove(File);
		
		for (auto &File : ImplicitDirectories)
		{
			auto Mapping = BackupManifest.m_Files(File);
			if (!Mapping.f_WasCreated())
				continue;
			
			auto &ManifestFile = *Mapping;
			ManifestFile.m_Attributes = CFile::fs_GetAttributes(CFile::fs_AppendPath(_Config.m_Root, File));
			fs_GetManifestFileProperties(_Config, File, ManifestFile);
		}
		
		return BackupManifest;
	}

	auto CBackupManagerClient::CInternal::f_UpdateManifest(CStr const &_FileName) -> TCContinuation<CUpdateManifestResult>
	{
		if (_FileName.f_IsEmpty())
			return fg_Explicit(CUpdateManifestResult{});
			
		TCContinuation<CUpdateManifestResult> Continuation;
		
		g_Dispatch(m_FileActor) > [_FileName, Config = m_Config]() mutable -> CUpdateManifestResult
			{
				auto AbsoluteFileName = CFile::fs_AppendPath(Config.m_Root, _FileName);
				
				if (!CFile::fs_FileExists(AbsoluteFileName, EFileAttrib_File | EFileAttrib_Link))
					return {{}, false};
				
				CBackupManagerBackup::CManifestFile ManifestFile;
				ManifestFile.m_Attributes = CFile::fs_GetAttributes(AbsoluteFileName);
				
				fs_GetManifestFileProperties(Config, _FileName, ManifestFile);
				
				CUpdateManifestResult Result = {fg_Move(ManifestFile), true};
				
				CStr Directory = CFile::fs_GetPath(_FileName);
				while (!Directory.f_IsEmpty())
				{
					auto &UpdatedDirectory = Result.m_UpdatedDirectories[Directory];
					UpdatedDirectory.m_ManifestFile.m_Attributes = CFile::fs_GetAttributes(AbsoluteFileName);
					fs_GetManifestFileProperties(Config, Directory, UpdatedDirectory.m_ManifestFile);
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
