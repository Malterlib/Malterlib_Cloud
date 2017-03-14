// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_LogAggregatorApp.h"

using namespace NMib;
using namespace NMib::NCloud::NLogAggregator;

class CLogAggregatorApplication : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudLogAggregator"
				, "Malterlib Cloud Log Aggregator"
				, "Aggregates logs from other applications"
				, []
				{
					return fg_ConstructActor<CLogAggregatorApp>();
				}
			}
		;
		return Daemon.f_Run();
	}	
};

DAppImplement(CLogAggregatorApplication);
