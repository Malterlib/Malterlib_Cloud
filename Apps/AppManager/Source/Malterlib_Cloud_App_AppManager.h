// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedDaemon>

namespace NMib
{
	namespace NCloud
	{
		namespace NAppManager
		{
			struct CAppManagerActor : public NConcurrency::CDistributedAppActor
			{
				CAppManagerActor();
				void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override
				{
					CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);
					o_CommandLine.f_SetProgramDescription
						(
							"Malterlib Cloud App Manager"
							, "Manages malterlib cloud applications by providing services such as encryption at rest and automatic updates." 
						)
					;
				}
				
				TCContinuation<void> fp_StartApp() override
				{
					return fg_Explicit();
				}				
			};
		}		
	}
}
