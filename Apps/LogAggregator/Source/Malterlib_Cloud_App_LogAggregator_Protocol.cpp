// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_LogAggregator.h"

namespace NMib::NCloud::NLogAggregator
{
	void CLogAggregatorServer::fp_Publish()
	{
		mp_ProtocolInterface.f_Publish<CLogAggregator>(mp_AppState.m_DistributionManager, this, CLogAggregator::mc_pDefaultNamespace);
	}
}
