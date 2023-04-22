// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Cloud/VersionManager>

namespace NMib::NCloud::NAppDistributionManager
{
	enum EDeployDestination
	{
		EDeployDestination_FileSystem
	};

	struct CDistributionSettings
	{
		bool operator == (CDistributionSettings const &_Right) const;

		CStr m_VersionManagerApplication;
		CStr m_RenameTemplate;
		TCSet<CStr> m_BranchWildcards;
		TCSet<CStr> m_Tags;
		TCSet<CStr> m_Platforms;
		TCSet<EDeployDestination> m_DeployDestinations;
	};

	struct CApplicationVersion
	{
		CVersionManager::CVersionIDAndPlatform m_VersionID;
		CVersionManager::CVersionInformation m_VersionInfo;
	};

	struct CDeployInfo
	{
		CStr m_SourceFile;
		CApplicationVersion m_Version;
		CDistributionSettings m_Settings;
		CStr m_Renamed;
		bool m_bElectron = false;
		CStr m_ElectronPlatform;
	};

	struct CDeployDestination : public CActor
	{
		virtual TCFuture<void> f_Deploy(CDeployInfo const &_DeployInfo) = 0;
	};

	struct CDeployDestination_FileSystem : public CDeployDestination
	{
		CDeployDestination_FileSystem();

		TCFuture<void> f_Deploy(CDeployInfo const &_DeployInfo) override;

	private:
		TCFuture<void> fp_Destroy() override;

		TCActor<CSeparateThreadActor> mp_FileActor;
	};

	struct CAppDistributionManagerActor : public CDistributedAppActor
	{
		CAppDistributionManagerActor();
		~CAppDistributionManagerActor();

	private:

		struct CDeployedVersionInfo
		{
			CTime m_Time;
			uint32 m_RetrySequence = 0;
		};

		struct CDistribution
		{
			CStr const &f_GetName() const
			{
				return TCMap<CStr, CDistribution>::fs_GetKey(*this);
			}

			// Permanent state
			CDistributionSettings m_Settings;
			TCMap<CVersionManager::CVersionIDAndPlatform, CDeployedVersionInfo> m_DeployedVersions;
			TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManager::CVersionInformation> m_TriedDeployVersions;

			// Ephemeral state
			TCMap<CVersionManager::CVersionIDAndPlatform, TCVector<TCActor<CDeployDestination>>> m_RunningDeploys;
		};
		
		struct CVersionManagerApplication;
		struct CVersionManagerState;
		
		struct CVersionManagerVersion
		{
			struct CCompareApplicationByTime
			{
				COrdering_Partial operator()(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const;
			};
			
			struct CCompareApplication
			{
				COrdering_Partial operator()(CVersionManagerVersion const &_Left, CVersionManagerVersion const &_Right) const;
				COrdering_Partial operator()(CVersionManager::CVersionIDAndPlatform const &_Left, CVersionManagerVersion const &_Right) const;
				COrdering_Partial operator()(CVersionManagerVersion const &_Left, CVersionManager::CVersionIDAndPlatform const &_Right) const;
			};

			CVersionManagerVersion(CVersionManagerState *_pVersionManager);
			~CVersionManagerVersion();
			void f_SetApplication(CVersionManagerApplication *_pApplication);
			CVersionManager::CVersionIDAndPlatform const &f_GetVersionID() const;
			
			CVersionManagerState *m_pVersionManager;
			CVersionManagerApplication *m_pApplication = nullptr;
			CVersionManager::CVersionInformation m_VersionInfo;
			TCAVLLink<> m_ApplicationTimeLink;
			TCAVLLink<> m_ApplicationLink;
		};
		
		struct CVersionManagerApplication
		{
			CVersionManagerApplication(CAppDistributionManagerActor &_This);
			CStr const &f_GetApplicationName() const
			{
				return TCMap<CStr, CVersionManagerApplication>::fs_GetKey(*this);
			}
			
			TCAVLTree<&CVersionManagerVersion::m_ApplicationTimeLink, CVersionManagerVersion::CCompareApplicationByTime> m_VersionsByTime;
			TCAVLTree<&CVersionManagerVersion::m_ApplicationLink, CVersionManagerVersion::CCompareApplication> m_Versions;
			CAppDistributionManagerActor &m_This;
		};

		struct CVersionManagerState
		{
			TCDistributedActor<CVersionManager> const &f_GetManager() const
			{
				return TCMap<TCDistributedActor<CVersionManager>, CVersionManagerState>::fs_GetKey(*this); 
			}
			
			TCMap<CStr, TCMap<CVersionManager::CVersionIDAndPlatform, CVersionManagerVersion>> m_Versions;
			
			CTrustedActorInfo m_HostInfo;
			CActorSubscription m_Subscription;
			mint m_SubscribeSequence = 0;
		};
		
		struct CVersionManagerDownloadState
		{
			TCActor<CFileTransferReceive> m_DownloadVersionReceive;
			CActorSubscription m_Subscription;
		};

		struct CVersionInformation
		{
			CVersionManager::CVersionInformation m_Information;
			TCVector<CStr> m_Files;
		};

		enum EFindVersionFlag
		{
			EFindVersionFlag_None = 0
			, EFindVersionFlag_RetryFailed = DBit(0)
			, EFindVersionFlag_ForAdd = DBit(1)
		};

		using CVersionsAvailableForUpdate = NContainer::TCMap<NStr::CStr, NContainer::TCVector<CApplicationVersion>>;

		TCFuture<void> fp_Destroy() override;

		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

		TCFuture<void> fp_StartApp(NEncoding::CEJSON const &_Params) override;
		TCFuture<void> fp_StopApp() override;
		TCFuture<void> fp_ReadState();

		TCFuture<uint32> fp_CommandLine_DistributionEnum(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_DistributionAdd(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_DistributionChangeSettings(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_DistributionRemove(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_ApplicationListAvailableVersions(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		static CStr fsp_DeployDestinationToString(EDeployDestination _Type);
		static EDeployDestination fsp_DeployDestinationFromString(CStr const &_Type);

		void fp_ParseSettings(CEJSON const &_Params, CDistributionSettings &o_Settings);
		CEJSON fp_SaveSettings(CDistributionSettings const &_Settings);
		void fp_SaveState(CDistribution const &_Distribution);

		TCFuture<void> fp_VersionManagerSubscribe(TCWeakDistributedActor<CVersionManager> const &_VersionManager);
		TCFuture<void> fp_VersionManagerAdded(TCDistributedActor<CVersionManager> const &_VersionManager, CTrustedActorInfo const &_Info);
		TCFuture<void> fp_VersionManagerRemoved(TCWeakDistributedActor<CActor> const &_VersionManager);
		TCFuture<CVersionsAvailableForUpdate> fp_GetAvailableVersions(CStr const &_Application);

		TCFuture<CVersionInformation> fp_DownloadApplicationFromManager
			(
				TCDistributedActor<CVersionManager> const &_Manager
				, CStr const &_ApplicationName
				, CVersionManager::CVersionIDAndPlatform const &_VersionID
				, CStr const &_DestinationDir
			)
		;
		
		TCFuture<CVersionInformation> fp_DownloadApplication
			(
				CStr const &_ApplicationName
				, CVersionManager::CVersionIDAndPlatform const &_VersionID
				, CStr const &_DestinationDir
			)
		;
		
		void fp_AutoUpdate_Update();

		TCActor<CDeployDestination> fp_CreateDeploy(EDeployDestination _Type);

		bool fsp_VersionMatchesSettings(CVersionManagerVersion const &_Version, CDistributionSettings const &_Settings);

		TCActor<CSeparateThreadActor> mp_FileActor;

		TCSequencer<TCSet<CStr>> mp_DistributeSequencer{"AppDistributionManagerActor DistributeSequencer", 8}; // Max 8 distributions at the same time

		TCMap<CStr, CDistribution> mp_Distributions;

		TCTrustedActorSubscription<CVersionManager> mp_VersionManagerSubscription;
		TCMap<CStr, CVersionManagerApplication> mp_VersionManagerApplications;
		TCMap<TCDistributedActor<CVersionManager>, CVersionManagerState> mp_VersionManagers;
		TCLinkedList<CVersionManagerDownloadState> mp_Downloads;

		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroy = fg_Construct();
	};
}
