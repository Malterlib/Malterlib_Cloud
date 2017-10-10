// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_TestApp.h"

using namespace NMib;

namespace NMib::NCloud::NTest
{
	CTestAppActor::CTestAppActor()
		: CDistributedAppActor(CDistributedAppActor_Settings("TestApp", false))
	{
	}

	void CTestAppActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
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

	void CTestAppActor::fp_PopulateAppInterfaceRegisterInfo(CDistributedAppInterfaceServer::CRegisterInfo &o_RegisterInfo, NEncoding::CEJSON const &_Params)
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

	TCContinuation<void> CTestAppActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		return fg_Explicit();
	}

	TCContinuation<void> CTestAppActor::fp_StopApp()
	{
		return fg_Explicit();
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_TestApp()
	{
		return fg_Construct<NTest::CTestAppActor>();
	}
}
