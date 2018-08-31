// Copyright (C) 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <string.h>

extern "C" char *basename(char *_pPath)
{
	NMib::NStr::CStr Path(_pPath);
	auto FilePart = CFile::fs_GetFile(Path);
	return strdup(FilePart.f_GetStr());
}
