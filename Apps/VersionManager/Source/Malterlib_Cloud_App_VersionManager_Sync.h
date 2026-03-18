// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/String/String>
#include <Mib/Container/Map>
#include <Mib/Container/Set>
#include <Mib/Container/Vector>
#include <Mib/Time/Time>
#include <Mib/File/File>
#include <Mib/Cloud/VersionManager>
#include <Mib/Concurrency/DistributedAppSensorReporter>

namespace NMib::NCloud::NVersionManager
{
	/// Configuration for a single sync source
	struct CSyncSourceConfig
	{
		NStr::CStr m_Name; // User-provided config key
		bool m_bEnabled = true;
		bool m_bPretend = false;
		NContainer::TCSet<NStr::CStr> m_SyncHosts; // Cryptographic host IDs to sync from
		NContainer::TCVector<NStr::CStr> m_ApplicationFilters;
		NContainer::TCVector<NStr::CStr> m_PlatformFilters;
		NContainer::TCVector<NStr::CStr> m_VersionFilters;
		NContainer::TCVector<NStr::CStr> m_TagFilters;
		NContainer::TCMap<NStr::CStr, NStr::CStr> m_CopyTagMappings; // SourceTag -> DestTag
		bool m_bSyncRetrySequence = false; // If true, use max(local, remote)
		NTime::CTime m_StartSyncDate; // Only sync versions >= this date (invalid = all)
		uint32 m_nMinSyncVersions = 0; // Minimum versions per app (ignores date)
		uint64 m_QueueSize = NFile::gc_IdealNetworkQueueSize;
	};

	/// Runtime state for a single sync source
	struct CSyncSourceState
	{
		NStr::CStr const &f_GetName() const
		{
			return NContainer::TCMap<NStr::CStr, CSyncSourceState>::fs_GetKey(*this);
		}

		CSyncSourceConfig m_Config;

		// Sensor state
		NConcurrency::CDistributedAppSensorReporter::CSensorReporter m_SensorReporter_Status;
		NContainer::TCMap<NStr::CStr, NStr::CStr> m_HostErrors; // Maps host ID to error message
		bool m_bSensorsRegistered = false;
	};

	/// Subscription state for a single host ID (shared across all sync configs that reference it)
	struct CSyncHostSubscription
	{
		NStr::CStr const &f_GetHostID() const
		{
			return NContainer::TCMap<NStr::CStr, CSyncHostSubscription>::fs_GetKey(*this);
		}

		NConcurrency::TCWeakDistributedActor<CVersionManager> m_Manager;
		NConcurrency::CActorSubscription m_Subscription;
	};

	/// Key for version sequencers to prevent concurrent sync of same version
	struct CSyncVersionKey
	{
		NStr::CStr m_Application;
		CVersionManager::CVersionIDAndPlatform m_VersionIDAndPlatform;

		auto operator <=> (CSyncVersionKey const &_Right) const noexcept = default;
	};

	/// State for tracking sync processing of a specific version
	struct CSyncVersionState
	{
		CSyncVersionState(NStr::CStr _SequencerDescription)
			: m_Sequencer(fg_Move(_SequencerDescription))
		{
		}

		NConcurrency::CSequencer m_Sequencer;
		// Origin ID of the notification chain currently being processed.
		// Used to detect sync loops - if a notification arrives with the same
		// origin ID that we're already processing, it indicates a loop in the
		// sync configuration.
		NStr::CStr m_CurrentOriginID;
		NContainer::TCSet<NStr::CStr> m_StoringTags;
		uint32 m_StoringRetrySequence = 0;
	};
}
