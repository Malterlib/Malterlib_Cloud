// Copyright (C) 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

struct CInitAllowInvalid
{
	CInitAllowInvalid()
	{
		NMib::NSys::fg_Process_AllowInvalidExit(true);
	}
};

static CInitAllowInvalid g_InitAllowInvalid;
