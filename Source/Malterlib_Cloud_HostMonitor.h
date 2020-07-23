// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedAppSensorStoreLocal>

namespace NMib::NCloud
{
	struct CHostMonitor : public NConcurrency::CActor
	{
		enum EInitFlag
		{
			EInitFlag_None = 0
			, EInitFlag_MonitorAllMounts = DMibBit(0)
		};

		struct CMonitorPathOptions
		{
			auto f_Tuple() const;
			bool operator == (CMonitorPathOptions const &_Other) const;

			NStr::CStr m_Path;
			uint64 m_WarnFree = TCLimitsInt<uint64>::mc_Max;
			uint64 m_CriticalFree = TCLimitsInt<uint64>::mc_Max;
			fp32 m_WarnFreePercent = 5.0; ///< Set to fp32::fs_Inf() to disable
			fp32 m_CriticalFreePercent = 1.0; ///< Set to fp32::fs_Inf() to disable
		};

		CHostMonitor(NConcurrency::TCActor<NConcurrency::CDistributedAppSensorStoreLocal> const &_SensorStore);
		~CHostMonitor();

		NConcurrency::TCFuture<void> f_Init(EInitFlag _Flags, fp64 _HostMonitorInterval);
		NConcurrency::TCFuture<NConcurrency::CActorSubscription> f_MonitorPath(CMonitorPathOptions const &_Options);

	private:

		NConcurrency::TCFuture<void> fp_Destroy();

		struct CInternal;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
