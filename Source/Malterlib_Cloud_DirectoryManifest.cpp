// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/EJSON>

#include "Malterlib_Cloud_DirectoryManifest.h"

namespace NMib::NCloud
{
	using namespace NFile;
	using namespace NContainer;
	using namespace NStr;
	
	auto CDirectoryManifestFile::fs_ParseSyncFlags(NEncoding::CEJSON const &_JSON) -> EDirectoryManifestSyncFlag
	{
		EDirectoryManifestSyncFlag Flags = EDirectoryManifestSyncFlag_None;
		
		for (auto &Flag : _JSON.f_Array())
		{
			if (Flag.f_String() == "Append")
				Flags |= EDirectoryManifestSyncFlag_Append;
			else if (Flag.f_String() == "TransactionLog")
				Flags |= EDirectoryManifestSyncFlag_TransactionLog;
			else
				DMibError(NStr::fg_Format("Unknown sync flag: {}", Flag.f_String()));
		}
		
		return Flags;
	}
	
	NEncoding::CEJSON CDirectoryManifestFile::fs_GenerateSyncFlags(EDirectoryManifestSyncFlag _Flags)
	{
		NEncoding::CEJSON Json;
		Json.f_Array();
		
		if (_Flags & EDirectoryManifestSyncFlag_Append)
			Json.f_Insert("Append");
		if (_Flags & EDirectoryManifestSyncFlag_TransactionLog)
			Json.f_Insert("TransactionLog");
		
		return Json;
	}
	
	bool CDirectoryManifestFile::operator == (CDirectoryManifestFile const &_Right) const
	{
		return NContainer::fg_TupleReferences
			(
				m_Digest
				, m_Length
				, m_WriteTime
				, m_SymlinkData
				, m_Attributes
				, m_Owner
				, m_Group
				, m_Flags
			)
			== NContainer::fg_TupleReferences
			(
				_Right.m_Digest
				, _Right.m_Length
				, _Right.m_WriteTime
				, _Right.m_SymlinkData
				, _Right.m_Attributes
				, _Right.m_Owner
				, _Right.m_Group
				, _Right.m_Flags
			)
		;
	}
	
	bool CDirectoryManifestFile::f_IsDirectory() const
	{
		return (m_Attributes & (NFile::EFileAttrib_Directory | NFile::EFileAttrib_Link)) == NFile::EFileAttrib_Directory;
	}
	
	bool CDirectoryManifestFile::f_IsFile() const
	{
		return (m_Attributes & (NFile::EFileAttrib_Directory | NFile::EFileAttrib_Link)) == NFile::EFileAttrib_None;
	}
	
	NStr::CStr const &CDirectoryManifestFile::f_GetFileName() const
	{
		return NContainer::TCMap<NStr::CStr, CDirectoryManifestFile>::fs_GetKey(*this);
	}
	
	NEncoding::CEJSON CDirectoryManifest::f_ToJSON() const
	{
		NEncoding::CEJSON JSON;
		JSON.f_Object();

		for (auto &File : m_Files)
		{
			auto &FileName = m_Files.fs_GetKey(File);
			auto &Entry = JSON[FileName];
			
			Entry["Digest"] = NContainer::CByteVector{File.m_Digest.f_GetData(), File.m_Digest.fs_GetSize()};
			Entry["Length"] = File.m_Length;
			Entry["WriteTime"] = File.m_WriteTime;
			Entry["SymlinkData"] = File.m_SymlinkData;
			Entry["Attributes"] = NFile::CFile::fs_AttribToJSON(File.m_Attributes);
			Entry["Owner"] = File.m_Owner;
			Entry["Group"] = File.m_Group;
			Entry["Flags"] = CDirectoryManifestFile::fs_GenerateSyncFlags(File.m_Flags);
		}
		
		return JSON;
	}

	CDirectoryManifest CDirectoryManifest::fs_FromJSON(NEncoding::CEJSON const &_JSON)
	{
		CDirectoryManifest Manifest;
		
		for (auto &File : _JSON.f_Object())
		{
			auto &OutFile = Manifest.m_Files[File.f_Name()];
			auto &ManifestJSON = File.f_Value();
			
			{
				auto Digest = ManifestJSON["Digest"].f_Binary();
				if (Digest.f_GetLen() != OutFile.m_Digest.fs_GetSize())
					DMibError("Digest is wrong size");
				NMem::fg_MemCopy(OutFile.m_Digest.f_GetData(), Digest.f_GetArray(), OutFile.m_Digest.fs_GetSize());
			}
			OutFile.m_Length = ManifestJSON["Length"].f_Integer();
			OutFile.m_WriteTime = ManifestJSON["WriteTime"].f_Date();
			OutFile.m_SymlinkData = ManifestJSON["SymlinkData"].f_String();
			OutFile.m_Attributes = NFile::CFile::fs_AttribFromJSON(ManifestJSON["Attributes"]);
			OutFile.m_Owner = ManifestJSON["Owner"].f_String();
			OutFile.m_Group = ManifestJSON["Group"].f_String();
			OutFile.m_Flags = CDirectoryManifestFile::fs_ParseSyncFlags(ManifestJSON["Flags"]);
		}
		
		return Manifest;
	}
	
	void CDirectoryManifest::fs_UpdateManifestFile
		(
			CDirectoryManifestConfig const &_Config
			, NStr::CStr const &_FileName
			, CDirectoryManifestFile &o_ManifestFile
			, NStr::CStr const &_OriginalPath
		)
	{
		using namespace NFile;
		using namespace NStr;

		auto OriginalFileName = CFile::fs_AppendPath(_Config.m_Root, _OriginalPath);
		o_ManifestFile.m_OriginalPath = _OriginalPath;
		
		if (o_ManifestFile.m_Attributes & EFileAttrib_Link)
		{
			o_ManifestFile.m_SymlinkData = CFile::fs_ResolveSymbolicLink(OriginalFileName);
			o_ManifestFile.m_Owner = CFile::fs_GetOwnerOnLink(OriginalFileName);
			o_ManifestFile.m_Group = CFile::fs_GetGroupOnLink(OriginalFileName);
			o_ManifestFile.m_Attributes = CFile::fs_GetAttributesOnLink(OriginalFileName);
			o_ManifestFile.m_WriteTime = CFile::fs_GetWriteTimeOnLink(OriginalFileName);
		}
		else
		{
			if (!(o_ManifestFile.m_Attributes & EFileAttrib_Directory))
			{
				CMibFilePos Length;
				o_ManifestFile.m_Digest = CFile::fs_GetFileChecksum_SHA256(OriginalFileName, &Length);
				o_ManifestFile.m_Length = Length;
			}
			o_ManifestFile.m_WriteTime = CFile::fs_GetWriteTime(OriginalFileName);
			o_ManifestFile.m_Owner = CFile::fs_GetOwner(OriginalFileName);
			o_ManifestFile.m_Group = CFile::fs_GetGroup(OriginalFileName);
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
	
	NStr::CStr CDirectoryManifestConfig::fs_ParseWildcard(NStr::CStr const &_Wildcard, bool &o_bRecursive)
	{
		auto WildcardFile = CFile::fs_GetFile(_Wildcard);
		o_bRecursive = false;
		if (WildcardFile.f_StartsWith("^"))
		{
			o_bRecursive = true;
			return CFile::fs_AppendPath(CFile::fs_GetPath(_Wildcard), WildcardFile.f_Extract(1)); 
		}
		else
			return _Wildcard;
	}
	
	namespace
	{
		void fg_ValidateWildcard(NStr::CStr const &_Wildcard, bool _bIsFileSearch)
		{
			if (CFile::fs_IsPathAbsolute(_Wildcard))
				DMibError("Wildcards cannot be absolute paths. They need to be relative to root.");
			
			if (_bIsFileSearch)
			{
				if (CFile::fs_GetPath(_Wildcard).f_FindChars("%*") >= 0)
					DMibError("Wildcards can only appear for file part of files searches. Directories cannot be wildcarded.");
			}
			
			if (CFile::fs_HasRelativeComponents(_Wildcard))
				DMibError("Wildcards cannot contain relative path components '..' or '.'");
		}

		template <typename tf_CContainer>
		void fg_ValidateWildcards(tf_CContainer const &_Container, bool _bIsFileSearch)
		{
			for (auto iWildcard = _Container.f_GetIterator(); iWildcard; ++iWildcard)
				fg_ValidateWildcard(iWildcard.f_GetKey(), _bIsFileSearch);
		}

		void fg_ValidateDestination(NStorage::TCOptional<NStr::CStr> const &_Destination)
		{
			if (!_Destination)
				return;
				
			if (CFile::fs_IsPathAbsolute(*_Destination))
				DMibError("Destinations cannot be absolute paths. They need to be relative to root.");

			{
				CStr Error;
				if (!CFile::fs_IsValidFilePath(*_Destination, Error))
					DMibError(fg_Format("Destinations cannot {}.", Error));
			}
			
			if (CFile::fs_HasRelativeComponents(*_Destination))
				DMibError("Destinations cannot contain relative path components '..' or '.'");
		}
		
		template <typename tf_CContainer>
		void fg_ValidateDestinations(tf_CContainer const &_Container)
		{
			for (auto &Destination : _Container)
				fg_ValidateDestination(Destination);
		}
	}
	
	CDirectoryManifest CDirectoryManifest::fs_GetManifest(CDirectoryManifestConfig const &_Config, NFunction::TCFunctionNoAlloc<void ()> const &_fCheckAbort)
	{
		CDirectoryManifest BackupManifest;

		TCMap<CStr, CStr> DestinationMapping;
		TCMap<CStr, CStr> DestinationMappingReverse;
		
		for (auto &Destination : _Config.m_IncludeWildcards)
		{
			auto &Wildcard = _Config.m_IncludeWildcards.fs_GetKey(Destination);
			
			bool bRecursive = false;
			auto WildcardParsed = CDirectoryManifestConfig::fs_ParseWildcard(Wildcard, bRecursive);
			
			CFile::CFindFilesOptions Options{CFile::fs_AppendPath(_Config.m_Root, WildcardParsed), bRecursive};
			CStr FindPath = CFile::fs_GetPath(Options.m_Path);
			Options.m_AttribMask = EFileAttrib_File | EFileAttrib_Directory | EFileAttrib_Link;
			Options.m_fCheckAbort = _fCheckAbort;
			
			for (auto &FoundFile : CFile::fs_FindFiles(Options))
			{
				CStr OriginalPath = CFile::fs_MakePathRelative(FoundFile.m_Path, _Config.m_Root);
				CStr RelativePath;
				if (Destination)
					RelativePath = CFile::fs_AppendPath(*Destination, CFile::fs_MakePathRelative(FoundFile.m_Path, FindPath));
				else
					RelativePath = OriginalPath;
				
				if (*DestinationMapping(RelativePath, FoundFile.m_Path) != FoundFile.m_Path)
					DMibError("Manifest config maps different files to the same destination");
				
				if (*DestinationMappingReverse(FoundFile.m_Path, RelativePath) != RelativePath)
					DMibError("Manifest config maps same file to different destinations");
				
				auto Mapping = BackupManifest.m_Files(RelativePath);
				if (!Mapping.f_WasCreated())
					continue;
				
				auto &ManifestFile = *Mapping;
				ManifestFile.m_Attributes = FoundFile.m_Attribs;
				ManifestFile.m_OriginalPath = OriginalPath;
			}
		}
		
		TCSet<CStr> ToRemove;
		TCMap<CStr, CDirectoryManifestConfig::CDestination> ImplicitDirectories;

		for (auto &ManifestFile : BackupManifest.m_Files)
		{
			auto &RelativePath = BackupManifest.m_Files.fs_GetKey(ManifestFile);
			if (fg_StrMatchesAnyWildcardInMap(RelativePath, _Config.m_ExcludeWildcards))
			{
				ToRemove[RelativePath];
				continue;
			}

			CStr Directory = CFile::fs_GetPath(RelativePath);
			while (!Directory.f_IsEmpty())
			{
				if (RelativePath != ManifestFile.m_OriginalPath)
				{
					if (ManifestFile.f_IsDirectory())
						ImplicitDirectories(Directory, ManifestFile.m_OriginalPath);
					else
						ImplicitDirectories(Directory, CFile::fs_GetPath(ManifestFile.m_OriginalPath));
				}
				else
					ImplicitDirectories[Directory] = CDirectoryManifestConfig::CDestination{};
				Directory = CFile::fs_GetPath(Directory);
			}
			
			fs_UpdateManifestFile(_Config, RelativePath, ManifestFile, ManifestFile.m_OriginalPath);
		}
		
		for (auto &File : ToRemove)
			BackupManifest.m_Files.f_Remove(File);
		
		for (auto &Destination : ImplicitDirectories)
		{
			auto &File = ImplicitDirectories.fs_GetKey(Destination);
			auto Mapping = BackupManifest.m_Files(File);
			if (!Mapping.f_WasCreated())
				continue;
			
			auto &ManifestFile = *Mapping;
			CStr OriginalPath;
			if (Destination)
				OriginalPath = *Destination;
			else
				OriginalPath = File;
			
			ManifestFile.m_Attributes = CFile::fs_GetAttributes(CFile::fs_AppendPath(_Config.m_Root, OriginalPath));
			fs_UpdateManifestFile(_Config, File, ManifestFile, OriginalPath);
		}
		
		return BackupManifest;
	}

	CDirectoryManifestConfig const &CDirectoryManifestConfig::f_Validate() const
	{
		if (!NFile::CFile::fs_IsPathAbsolute(m_Root))
			DMibError("Root path is not absolute");
		
		fg_ValidateWildcards(m_IncludeWildcards, true);
		fg_ValidateDestinations(m_IncludeWildcards);
		fg_ValidateWildcards(m_ExcludeWildcards, false);
		fg_ValidateWildcards(m_AddSyncFlagsWildcards, false);
		fg_ValidateWildcards(m_RemoveSyncFlagsWildcards, false);
		
		return *this;
	}
}
