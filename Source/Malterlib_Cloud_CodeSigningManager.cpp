// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_CodeSigningManager.h"

namespace NMib::NCloud
{
	CCodeSigningManager::CCodeSigningManager()
	{
		DMibPublishActorFunction(CCodeSigningManager::f_SignFiles);
	}

	CCodeSigningManager::~CCodeSigningManager() = default;
}
