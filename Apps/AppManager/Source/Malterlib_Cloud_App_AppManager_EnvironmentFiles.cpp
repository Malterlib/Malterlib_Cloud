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

	// Deletes a file of the previous version of an application in _Directory, _File being relative to it. A link is deleted
	// itself, and a file below a link is kept, since the application can have created links to anywhere. A directory is
	// only deleted when empty, to keep the untracked contents
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
}
