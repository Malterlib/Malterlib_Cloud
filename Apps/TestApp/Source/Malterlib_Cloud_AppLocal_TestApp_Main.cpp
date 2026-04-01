// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Concurrency/DistributedDaemon>

#include <Mib/Cloud/App/TestApp>

using namespace NMib;

struct CTestAppApp : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudTestApp"
				, "Malterlib Cloud Test App"
				, "Test"
				, []
				{
					return NMib::NCloud::fg_ConstructApp_TestApp();
				}
			}
		;
		return Daemon.f_Run();
	}
};

DAppImplement(CTestAppApp);
