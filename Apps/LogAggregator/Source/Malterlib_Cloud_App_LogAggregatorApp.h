// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>

namespace NMib::NCloud::NLogAggregator
{
	struct CLogAggregatorServer;
	
	struct CLogAggregatorApp : public CDistributedAppActor
	{
		CLogAggregatorApp();
		~CLogAggregatorApp();

	private:
		TCContinuation<void> fp_StartApp(NEncoding::CEJSON const &_Params) override;
		TCContinuation<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override; 
		
		TCActor<CLogAggregatorServer> mp_Server;
	};
}
