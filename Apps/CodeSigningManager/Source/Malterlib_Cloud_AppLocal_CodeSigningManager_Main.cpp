// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>

#include "Malterlib_Cloud_App_CodeSigningManager.h"

using namespace NMib;
using namespace NMib::NCloud::NCodeSigningManager;

class CCodeSigningManagerApplication : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudCodeSigningManager"
				, "Malterlib Cloud Code Signing Manager"
				, "Manages issuing and reissuing of code signing certificates as well as signing of executables"
				, []
				{
					return fg_ConstructActor<CCodeSigningManagerActor>();
				}
			}
		;

		return Daemon.f_Run();
	}
};

DAppImplement(CCodeSigningManagerApplication);
