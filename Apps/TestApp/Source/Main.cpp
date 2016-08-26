// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Concurrency/DistributedDaemon>

using namespace NMib;

namespace NMib
{
	namespace NCloud
	{
		namespace NTest
		{
			struct CTestAppActor : public NConcurrency::CDistributedAppActor
			{
				CTestAppActor()
					: CDistributedAppActor(CDistributedAppActor_Settings("TestApp", false))
				{
				}
				
				void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
				{
					CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);
					o_CommandLine.f_SetProgramDescription
						(
							"Test App"
							, "Test App." 
						)
					;
				}
				
				TCContinuation<void> fp_StartApp(NEncoding::CEJSON const &_Params)
				{
					return fg_Explicit();
				}				

				TCContinuation<void> fp_StopApp()
				{
					return fg_Explicit();
				}				
			};
		}		
	}
}

class CAppManager : public CApplication
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
					return fg_ConstructActor<NMib::NCloud::NTest::CTestAppActor>();
				}
			}
		;
		return Daemon.f_Run();
	}	
};

DAppImplement(CAppManager);
