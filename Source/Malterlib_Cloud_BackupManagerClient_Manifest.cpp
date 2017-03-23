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
			
			for (auto &FoundFile : CFile::fs_FindFilesEx(CFile::fs_AppendPath(_Config.m_Root, WildcardParsed), EFileAttrib_File | EFileAttrib_Link, bRecursive, false))
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

		for (auto &ManifestFile : BackupManifest.m_Files)
		{
			auto &RelativePath = BackupManifest.m_Files.fs_GetKey(ManifestFile);
			if (fs_MatchesAnyWildcard(RelativePath, _Config.m_ExcludeWildcards))
			{
				ToRemove[RelativePath];
				continue;
			}
			
			fs_GetManifestFileProperties(_Config, RelativePath, ManifestFile);
		}
		
		for (auto &File : ToRemove)
			BackupManifest.m_Files.f_Remove(File);
		
		return BackupManifest;
	}
	
	auto CBackupManagerClient::CInternal::f_UpdateManifest(CStr const &_FileName) -> TCContinuation<CUpdateManifestResult>
	{
		TCContinuation<CUpdateManifestResult> Continuation;
		
		g_Dispatch(m_FileActor) > [_FileName, Config = m_Config]() mutable -> CUpdateManifestResult
			{
				auto AbsoluteFileName = CFile::fs_AppendPath(Config.m_Root, _FileName);
				
				if (!CFile::fs_FileExists(AbsoluteFileName, EFileAttrib_File | EFileAttrib_Link))
					return {false, {}};
				
				CBackupManagerBackup::CManifestFile ManifestFile;
				ManifestFile.m_Attributes = CFile::fs_GetAttributes(AbsoluteFileName);
				
				fs_GetManifestFileProperties(Config, _FileName, ManifestFile);
					
				return {true, fg_Move(ManifestFile)};
			}
			> Continuation / [this, _FileName, Continuation](CUpdateManifestResult &&_Result)
			{
				if (!_Result.m_bExists)
					m_Manifest.m_Files.f_Remove(_FileName);
				else
					m_Manifest.m_Files[_FileName] = fg_Move(_Result.m_ManifestFile);
				
				Continuation.f_SetResult(fg_Move(_Result));
			}
		;
		
		return Continuation;
	}
}
