// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/DistributedApp>

using namespace NMib;

namespace NMib::NCloud::NTest
{
	struct CTestAppActor : public NConcurrency::CDistributedAppActor
	{
		CTestAppActor();

	protected:
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
		void fp_PopulateAppInterfaceInfo
			(
				CDistributedAppInterfaceServer::CRegisterInfo &o_RegisterInfo
				, CDistributedAppInterfaceServer::CConfigFiles &o_ConfigFiles
				, NEncoding::CEJSONSorted const &_Params
			)
			override
		;
		TCFuture<void> fp_StartApp(NEncoding::CEJSONSorted const &_Params) override;
		TCFuture<void> fp_StopApp() override;
	};
}

