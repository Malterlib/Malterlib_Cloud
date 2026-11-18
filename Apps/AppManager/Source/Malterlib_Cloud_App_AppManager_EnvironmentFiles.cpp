// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_AppManager.h"

#include <Mib/File/FileBeneath>

#ifndef DPlatformFamily_Windows
#	include <Mib/Core/PlatformSpecific/PosixErrNo>
#	include <stdlib.h>
#	include <sys/stat.h>
#endif

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

	// Deletes a file of the previous version of an application in _Directory, _File being relative to it. A link is deleted
	// itself, and a file below a link is kept, since the application can have created links to anywhere. A directory is
	// only deleted when empty, to keep the untracked contents. On Windows every reparse point counts as a link, also those
	// that don't redirect, like the placeholders of cloud file providers, which the application directories of the AppManager
	// don't hold; deleting one that isn't empty fails the update like any other file the update can't delete. The checks only
	// avoid the links that are already there, not one that a running process of the application's account swaps in after them:
	// an application on the host isn't contained from its own account, which is what container and VM environments are for
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

	// Gives the directories and files in _RelativeDirectories of _Directory, the directories they are in and _Directory itself to _User and _Group; an empty
	// name keeps that one. Root changes the owners, and the application's account can have replaced the directories in _Directory with links to
	// directories of other accounts, which aren't followed: a directory below a link is skipped, and CFileBeneath refuses a link in the path and gives
	// a link that is the directory itself the owner. CFileBeneath also stops at a link swapped in while this runs, which the other root operations in
	// the directory don't, see fg_AppManager_ReplaceApplicationFiles. _LinkDirectory is _Directory or a directory that it is in, whose name is in a
	// directory of the AppManager, where an administrator can have made it a link, for example to another volume, which CFileBeneath refuses as a
	// root. Only that link is followed. A child application is in the directory of its parent, which belongs to the parent's account, so that is its
	// _LinkDirectory. Not on Windows, which doesn't change owners
	void fg_AppManager_SetApplicationDirectoryOwners
		(
			NStr::CStr const &_LinkDirectory
			, NStr::CStr const &_Directory
			, NContainer::TCSet<NStr::CStr> const &_RelativeDirectories
			, NStr::CStr const &_User
			, NStr::CStr const &_Group
		)
	{
		using namespace NMib::NStr;
		using namespace NMib::NFile;

		if (_User.f_IsEmpty() && _Group.f_IsEmpty())
			return;

		CStr Prefix;
		if (_Directory != _LinkDirectory)
		{
			if (!_Directory.f_StartsWith(_LinkDirectory + "/"))
				DMibError("The application directory '{}' is not in '{}'"_f << _Directory << _LinkDirectory);

			Prefix = _Directory.f_Extract(_LinkDirectory.f_GetLen() + 1);
		}

#ifdef DPlatformFamily_Windows
		CStr Directory = _LinkDirectory;
		for (umint iLink = 0; CFile::fs_FileExists(Directory, EFileAttrib_Link); ++iLink)
		{
			if (iLink == 32)
				DMibError("The application directory '{}' is a chain of too many links"_f << _LinkDirectory);

			CStr Target = CFile::fs_ResolveSymbolicLink(Directory);
			Directory = CFile::fs_IsPathAbsolute(Target) ? Target : CStr(CFile::fs_GetPath(Directory) + "/" + Target);
		}
#else
		// The links that lead to the application directory are resolved by the file system, in the order of the path: the file functions
		// normalize a .. in a path as text, which after another link names a different directory
		CStr Directory;
		{
			ch8 *pResolved = realpath(_LinkDirectory.f_GetStr(), nullptr);
			if (!pResolved)
				DMibError(NMib::NPlatform::fg_FormatErrno(CStr::CFormat("realpath('{}') when resolving the application directory") << _LinkDirectory, errno));

			Directory = pResolved;
			free(pResolved);
		}
#endif

		NContainer::TCSet<CStr> Directories;
		Directories[Prefix];
		for (auto &RelativeDirectory : _RelativeDirectories)
		{
			CStr Relative = Prefix;
			if (!RelativeDirectory.f_IsEmpty())
				Relative = Prefix.f_IsEmpty() ? RelativeDirectory : CStr(Prefix / RelativeDirectory);

			// The real directories above a link still change owner
			bool bLinked = true;
			for (CStr Path = Relative; Path != Prefix && !Path.f_IsEmpty(); Path = CFile::fs_GetPath(Path))
			{
				if (bLinked)
					bLinked = fg_AppManager_HasLinkedParent(Directory, Path + "/");

				if (!bLinked)
					Directories[Path];
			}
		}

		for (auto &Path : Directories)
			CFileBeneath::fs_SetOwnerAndGroup(Directory, Path, _User, _Group);
	}

#ifndef DPlatformFamily_Windows
	// Whether the regular file at _Path is to get _User and _Group from fg_AppManager_SetApplicationDirectoryOwners, an empty name keeping that
	// one. A file that has them already keeps them, since on Linux every owner change clears the file capabilities, also one to the same owner. A
	// file with other links shares its owner with names that can be outside the application directory, so it keeps its owner too
	bool fg_AppManager_ApplicationFileNeedsOwners(NStr::CStr const &_Path, NStr::CStr const &_User, NStr::CStr const &_Group)
	{
		using namespace NMib::NStr;
		using namespace NMib::NFile;

		struct stat Stat;
		if (::lstat(_Path.f_GetStr(), &Stat) != 0)
			DMibError(NMib::NPlatform::fg_FormatErrno(CStr::CFormat("lstat('{}') when checking the owner of an application file") << _Path, errno));

		if (!S_ISREG(Stat.st_mode))
			return false;

		bool bChangeUser = !_User.f_IsEmpty() && CFile::fs_GetOwnerOnLink(_Path) != _User;
		bool bChangeGroup = !_Group.f_IsEmpty() && CFile::fs_GetGroupOnLink(_Path) != _Group;
		if (!bChangeUser && !bChangeGroup)
			return false;

		if (Stat.st_nlink > 1)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Keeping the owner of application file '{}', which has other links", _Path);
			return false;
		}

		return true;
	}
#endif

	// The path of a low port executable to give setcap, or empty when _Executable, relative to _Directory, is missing, is a link or is below one.
	// setcap runs as root, and the application's account can have replaced the files and directories in _Directory with links to files of other
	// accounts, which would get the capability. setcap resolves the path again after this check, which only avoids the links that are already
	// there: an application on the host isn't contained from its own account, see fg_AppManager_ReplaceApplicationFiles
	NStr::CStr fg_AppManager_LowPortExecutablePath(NStr::CStr const &_Directory, NStr::CStr const &_Executable)
	{
		using namespace NMib::NStr;
		using namespace NMib::NFile;

		CStr Path = _Directory / _Executable;
		if (fg_AppManager_HasLinkedParent(_Directory, _Executable) || CFile::fs_FileExists(Path, EFileAttrib_Link))
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Not setting the capabilities of the low port executable '{}', which is a link or below one", Path);
			return {};
		}

		if (!CFile::fs_FileExists(Path, EFileAttrib_File))
			return {};

		return Path;
	}

	// Replaces the files of the previous version of an application in _Directory with the files and links of the new version in
	// _SourceDirectory, which are moved. _OldFiles and _Files are relative to their directories. Root moves and deletes the files
	// by path after checking them for links, so only the links that are already there are avoided, not one that a running process
	// of the application's account swaps in after the checks: an application on the host, without an environment or in a Local
	// environment, isn't contained from its own account, which is what container and VM environments are for. Returns the entries of _Files that
	// are installed: the update scripts can remove files from _SourceDirectory, like a file whose copy in _Directory they keep, and the next
	// update deletes the returned files. A file of the previous version that the new version has too, but whose copy the scripts removed, keeps
	// its copy in _Directory and stays installed
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

		// The update scripts of the application account can replace the directories of an unpack in the application directory with links to files
		// of other accounts, which the move would bring into the application directory
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

		NContainer::TCSet<CStr> NewFiles;
		for (auto &File : _Files)
			NewFiles[File];

		NContainer::TCSet<CStr> OldFiles;
		for (auto &File : _OldFiles)
			OldFiles[File];

		// A file that the application created where the new version has a directory fails the update, before anything of the old version is
		// deleted. The old files are deleted, and a linked parent is replaced with a real directory, so neither is in the way. An entry that the
		// update scripts removed from _SourceDirectory isn't installed, so nothing of it needs a directory. A case-insensitive file system finds
		// an old file under another spelling, which is the same file
		auto fIsOldFileSpelledOtherwise = [&](CStr const &_Path, CStr const &_DestinationPath)
			{
				for (auto &OldFile : _OldFiles)
				{
					if (OldFile.f_CmpNoCase(_Path) != 0 || !CFile::fs_FileExists(_Directory / OldFile, EFileAttrib_File | EFileAttrib_Link))
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
			if (!CFile::fs_FileExists(SourcePath, EFileAttrib_File | EFileAttrib_Link | EFileAttrib_Directory))
				continue;

			bool bDirectory = !CFile::fs_FileExists(SourcePath, EFileAttrib_File | EFileAttrib_Link) && CFile::fs_FileExists(SourcePath, EFileAttrib_Directory);

			for (CStr Path = bDirectory ? File : CStr(CFile::fs_GetPath(File)); !Path.f_IsEmpty(); Path = CFile::fs_GetPath(Path))
			{
				if (OldFiles.f_FindEqual(Path) || fg_AppManager_HasLinkedParent(_Directory, Path))
					continue;

				CStr DestinationPath = _Directory / Path;
				if (CFile::fs_FileExists(DestinationPath, EFileAttrib_File) && !CFile::fs_FileExists(DestinationPath, EFileAttrib_Link) && !fIsOldFileSpelledOtherwise(Path, DestinationPath))
					DMibError(fg_Format("The application file '{}' is where the update has a directory", Path));
			}
		}

		// A file is kept only when its copy is still there; one that is gone isn't installed, so that the next update doesn't take a file that the
		// application creates at its path for a file of the package. The update scripts are trusted content of the package, which keep a file by
		// removing its copy from the unpack under its exact name in the new version, also on a case-insensitive file system; a script that
		// replaces a directory of the unpack with a file fails the update where the file can't replace the directory
		NContainer::TCSet<CStr> KeptFiles;
		for (auto &File : _OldFiles)
		{
			if
			(
				NewFiles.f_FindEqual(File)
				&& !CFile::fs_FileExists(_SourceDirectory / File, EFileAttrib_File | EFileAttrib_Link | EFileAttrib_Directory)
				&& CFile::fs_FileExists(_Directory / File, EFileAttrib_File | EFileAttrib_Link | EFileAttrib_Directory)
			)
			{
				KeptFiles[File];
				continue;
			}

			fg_AppManager_DeleteApplicationFile(_Directory, File);
		}

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
				// The next update deletes the directory, so it is created, and a file of the application in its place fails the update instead
				// of being taken for it. The linked parents are replaced first, so the check doesn't look beyond a link
				if (CFile::fs_FileExists(SourcePath, EFileAttrib_Directory))
				{
					fg_AppManager_CreateApplicationDirectory(_Directory, CFile::fs_GetPath(File));

					CStr DestinationPath = _Directory / File;
					if (CFile::fs_FileExists(DestinationPath, EFileAttrib_File) && !CFile::fs_FileExists(DestinationPath, EFileAttrib_Link))
						DMibError(fg_Format("The application file '{}' is where the update has a directory", File));

					fg_AppManager_CreateApplicationDirectory(_Directory, File);
					Installed.f_Insert(File);
				}
				else if (KeptFiles.f_FindEqual(File))
					Installed.f_Insert(File);
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
