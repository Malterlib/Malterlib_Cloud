// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/Encoding/JSONShortcuts>

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
				
				void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override
				{
					CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);
					o_CommandLine.f_SetProgramDescription
						(
							"Test App"
							, "Test App." 
						)
					;
					o_CommandLine.f_RegisterGlobalOptions
						(
							{
								"UpdateType?"_=
								{
									"Names"_= {"--update-type"}
									,"Type"_= COneOf{"Independent", "OneAtATime", "AllAtOnce"}
									, "Description"_= "Override the update type for the application"
								}
							}
						)
					;
				}
				
				void fp_PopulateAppInterfaceRegisterInfo(CDistributedAppInterfaceServer::CRegisterInfo &o_RegisterInfo, NEncoding::CEJSON const &_Params) override
				{
					if (auto pValue = _Params.f_GetMember("UpdateType", EJSONType_String))
					{
						CStr UpdateType = pValue->f_String();
						if (UpdateType == "Independent")
							o_RegisterInfo.m_UpdateType = EDistributedAppUpdateType_Independent; 
						else if (UpdateType == "OneAtATime")
							o_RegisterInfo.m_UpdateType = EDistributedAppUpdateType_OneAtATime; 
						else if (UpdateType == "AllAtOnce")
							o_RegisterInfo.m_UpdateType = EDistributedAppUpdateType_AllAtOnce; 
					}
				}
				
				TCContinuation<void> fp_StartApp(NEncoding::CEJSON const &_Params) override
				{
					return fg_Explicit();
				}				

				TCContinuation<void> fp_StopApp() override
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
