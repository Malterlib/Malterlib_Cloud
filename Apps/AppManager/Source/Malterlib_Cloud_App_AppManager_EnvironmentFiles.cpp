// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud
{
	// True when a directory that _File, relative to _Directory, is in is a link below _Directory
	bool fg_AppManager_HasLinkedParent(NStr::CStr const &_Directory, NStr::CStr const &_File)
	{
		using namespace NMib::NStr;
		using namespace NMib::NFile;

		auto Components = _File.f_Split("/");

		CStr Path = _Directory;
		for (umint iComponent = 0; iComponent + 1 < Components.f_GetLen(); ++iComponent)
		{
			Path = Path / Components[iComponent];
			if (CFile::fs_FileExists(Path, EFileAttrib_Link))
				return true;
		}

		return false;
	}

	// Deletes _File, relative to _Directory, of the previous version of an application. A link is deleted itself, a file below a link is kept
	// and a directory is deleted only when empty. On Windows every reparse point counts as a link
	void fg_AppManager_DeleteApplicationFile(NStr::CStr const &_Directory, NStr::CStr const &_File)
	{
		using namespace NMib::NStr;
		using namespace NMib::NFile;

		CStr FullPath = _Directory / _File;

		if (fg_AppManager_HasLinkedParent(_Directory, _File))
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Keeping old application file '{}', which is below a link", FullPath);
			return;
		}

		if (CFile::fs_FileExists(FullPath, EFileAttrib_Link))
			CFile::fs_DeleteFile(FullPath);
		else if (CFile::fs_FileExists(FullPath, EFileAttrib_Directory))
		{
			// The deletion fails for a directory that isn't empty, which can hold any number of files the application created, so it isn't
			// listed first
			try
			{
				CFile::fs_DeleteDirectory(FullPath);
			}
			catch (CExceptionFile const &_Exception)
			{
				[[maybe_unused]] auto &Exception = _Exception;
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Kept old application directory '{}': {}", FullPath, Exception);
			}
		}
		else if (CFile::fs_FileExists(FullPath, EFileAttrib_File))
			CFile::fs_DeleteFile(FullPath);
	}

	// Creates _RelativeDirectory in _Directory with a real directory for every component. The application can have replaced
	// a directory with a link to anywhere, which the files written below it would follow, so such links are deleted
	void fg_AppManager_CreateApplicationDirectory(NStr::CStr const &_Directory, NStr::CStr const &_RelativeDirectory)
	{
		using namespace NMib::NStr;
		using namespace NMib::NFile;

		CStr Path = _Directory;
		for (auto &Component : _RelativeDirectory.f_Split("/"))
		{
			if (Component.f_IsEmpty())
				continue;

			Path = Path / Component;

			if (CFile::fs_FileExists(Path, EFileAttrib_Link))
				CFile::fs_DeleteFile(Path);

			if (!CFile::fs_FileExists(Path, EFileAttrib_Directory))
				CFile::fs_CreateDirectory(Path);
		}
	}

	// Moves the files and links of the new version in _SourceDirectory over those of the previous version in _Directory; _OldFiles and _Files are
	// relative to their directories. Avoids the links below _Directory that exist when it checks, not a link at or above _Directory. Returns the
	// installed entries of _Files, which leave out the entries missing from _SourceDirectory
	auto fg_AppManager_ReplaceApplicationFiles
		(
			NStr::CStr const &_Directory
			, NStr::CStr const &_SourceDirectory
			, NContainer::TCVector<NStr::CStr> const &_OldFiles
			, NContainer::TCVector<NStr::CStr> const &_Files
		)
		-> NContainer::TCVector<NStr::CStr>
	{
		using namespace NMib::NStr;
		using namespace NMib::NFile;

		// The update scripts of the application account can replace the parent directories of the unpacked files with links to files of other
		// accounts, which the move would bring into the application directory
		CStr LinkCheckRoot = _SourceDirectory;
		CStr SourcePrefix;
		if (_SourceDirectory.f_StartsWith(_Directory + "/"))
		{
			LinkCheckRoot = _Directory;
			SourcePrefix = _SourceDirectory.f_Extract(_Directory.f_GetLen() + 1) + "/";
		}

		for (auto &File : _Files)
		{
			if (fg_AppManager_HasLinkedParent(LinkCheckRoot, SourcePrefix + File))
				DMibError(fg_Format("The unpacked application file '{}' is below a link", File));
		}

		NContainer::TCSet<CStr> OldFiles;
		for (auto &File : _OldFiles)
			OldFiles[File];

		// A file of the application where the new version has a directory fails the update before anything is deleted. Old files and linked
		// parents aren't in the way, and a case-insensitive file system can find an old file under another spelling, which the identity of the
		// file tells. Entries missing from _SourceDirectory, which the update scripts can remove, install nothing
		auto fIsOldFileSpelledOtherwise = [&](CStr const &_DestinationPath)
			{
				for (auto &OldFile : _OldFiles)
				{
					if (!CFile::fs_FileExists(_Directory / OldFile, EFileAttrib_File | EFileAttrib_Link))
						continue;

					if (CFile::fs_GetUniqueIdentifierOnLink(_Directory / OldFile) == CFile::fs_GetUniqueIdentifierOnLink(_DestinationPath))
						return true;
				}

				return false;
			}
		;

		for (auto &File : _Files)
		{
			CStr SourcePath = _SourceDirectory / File;
			bool bDirectory = !CFile::fs_FileExists(SourcePath, EFileAttrib_File | EFileAttrib_Link) && CFile::fs_FileExists(SourcePath, EFileAttrib_Directory);
			if (!bDirectory && !CFile::fs_FileExists(SourcePath, EFileAttrib_File | EFileAttrib_Link))
				continue;

			for (CStr Path = bDirectory ? File : CStr(CFile::fs_GetPath(File)); !Path.f_IsEmpty(); Path = CFile::fs_GetPath(Path))
			{
				if (OldFiles.f_FindEqual(Path) || fg_AppManager_HasLinkedParent(_Directory, Path))
					continue;

				CStr DestinationPath = _Directory / Path;
				if (CFile::fs_FileExists(DestinationPath, EFileAttrib_File) && !CFile::fs_FileExists(DestinationPath, EFileAttrib_Link) && !fIsOldFileSpelledOtherwise(DestinationPath))
					DMibError(fg_Format("The application file '{}' is where the update has a directory", Path));
			}
		}

		for (auto &File : _OldFiles)
			fg_AppManager_DeleteApplicationFile(_Directory, File);

#ifdef DPlatformFamily_Windows
		if (CSystem::ms_PlatformVersion <= 10'0'017763)
			NSys::fg_Thread_Sleep(5.0); // Try to work around access denied below when file is renamed
#endif

		NContainer::TCVector<CStr> Installed;
		for (auto &File : _Files)
		{
			CStr SourcePath = _SourceDirectory / File;
			if (!CFile::fs_FileExists(SourcePath, EFileAttrib_File | EFileAttrib_Link))
			{
				// The next update deletes the directory, so a file of the application at its path fails the update instead of being recorded as
				// the directory. The parents become real directories first, so the check doesn't look through a link
				if (CFile::fs_FileExists(SourcePath, EFileAttrib_Directory))
				{
					fg_AppManager_CreateApplicationDirectory(_Directory, CFile::fs_GetPath(File));

					CStr DestinationPath = _Directory / File;
					if (CFile::fs_FileExists(DestinationPath, EFileAttrib_File) && !CFile::fs_FileExists(DestinationPath, EFileAttrib_Link))
						DMibError(fg_Format("The application file '{}' is where the update has a directory", File));

					fg_AppManager_CreateApplicationDirectory(_Directory, File);
#ifndef DPlatformFamily_Windows
					// The unpack normalized the permissions of the packaged directory, which can keep other accounts out of it
					CFile::fs_SetAttributes(DestinationPath, (CFile::fs_GetAttributes(SourcePath) & ~EFileAttrib_Directory) | EFileAttrib_UnixAttributesValid);
#endif
					Installed.f_Insert(File);
				}
				continue;
			}

			// The rename replaces a link at the destination itself
			fg_AppManager_CreateApplicationDirectory(_Directory, CFile::fs_GetPath(File));
			CFile::fs_AtomicReplaceFile(SourcePath, _Directory / File);
			Installed.f_Insert(File);
		}

		return Installed;
	}
}
