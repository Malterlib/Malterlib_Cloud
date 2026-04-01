// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedApp>

#include "Malterlib_Cloud_AppLocal_CloudClient.h"

using namespace NMib;
using namespace NMib::NCloud::NCloudClient;

struct CCloudClient : public CApplication
{
	aint f_Main()
	{
		return fg_RunApp
			(
				[]
				{
					return fg_ConstructActor<CCloudClientAppLocalActor>();
				}
			)
		;
	}
};

DAppImplement(CCloudClient);
