// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NAppManager
		{
			CAppManagerActor::CAppManagerActor()
				: CDistributedAppActor(CDistributedAppActor_Settings("AppManager", false))
			{
			}
		}		
	}
}
