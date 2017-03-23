// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Storage/Variant>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/ActorFunctor>
#include <Mib/Cloud/BackupManager>
#include <Mib/Concurrency/DistributedActorTrustManager>

namespace NMib::NCloud
{
	namespace NPrivate
	{
		struct CBackupManagerClient_Instance;
	}
	
	/// \brief Implements a client that backs up through backup manager interface
	struct CBackupManagerClient : public NConcurrency::CActor
	{
		using EManifestSyncFlag = CBackupManagerBackup::EManifestSyncFlag;
		
		struct CConfig /// \brief Config for backup manager client. \headerfile Mib/Cloud/BackupManagerClient
		{
			CConfig const &f_Validate() const; ///< Throws exception if settings are invalid
			
			NStr::CStr m_BackupIdentifier;
			
			NStr::CStr m_Root;																		///< The root directory of the backup. 
			NContainer::TCSet<NStr::CStr> m_IncludeWildcards;										///< \brief Relative to m_Root. This is a file search. Only file name can have wildcards.
																									/// Use ^ in the beginning of the file path to create a recursive search.
			NContainer::TCSet<NStr::CStr> m_ExcludeWildcards;										///< Relative to m_Root. Evaluated after include wild cards as a filtering step.
			NContainer::TCMap<NStr::CStr, EManifestSyncFlag> m_AddSyncFlagsWildcards;				///< Relative to m_Root.
			NContainer::TCMap<NStr::CStr, EManifestSyncFlag> m_RemoveSyncFlagsWildcards;			///< Relative to m_Root. Evaluated after m_AddSyncFlagsWildcards.
			NTime::CTimeSpan m_NewBackupInterval = NTime::CTimeSpanConvert::fs_CreateDaySpan(1);	///< Interval for creating new full backup snapshots. Set to 0 to disable.
			NStr::CStr m_LogCategory = "Backup";													///< The category to do logging under.
		};
		
		enum ENotification /// Notification from backup manager client
		{
			ENotification_None = 0						///< Used to specify no notification when subscribing. \sa f_SubscribeNotifications
			, ENotification_BackupAborted = DMibBit(0)	///< Backup was aborted remotely. \sa CNotification_BackupAborted
			, ENotification_BackupFailed = DMibBit(1)	///< Backup failed. \sa CNotification_BackupFailed
			, ENotification_FileFinished = DMibBit(2)	///< A file finished transferring to backup manager. \sa CNotification_FileFinished
			, ENotification_Quiescent = DMibBit(3)		///< The backup is quiescent. All currently known files have finished transferring. \sa CNotification_Quiescent
		};
		
		struct CNotification_BackupAborted /// \brief Notification info for #ENotification_BackupAborted. \headerfile Mib/Cloud/BackupManagerClient
		{
		};
		
		struct CNotification_BackupFailed /// \brief Notification info for #ENotification_BackupFailed. \headerfile Mib/Cloud/BackupManagerClient
		{
			NStr::CStr m_ErrorMessage; ///< The error from exception that caused the backup to fail
		};
		
		struct CNotification_FileFinished /// \brief Notification info for #ENotification_FileFinished. \headerfile Mib/Cloud/BackupManagerClient
		{
			NStr::CStr m_FileName; ///< The file that finished backing up. Relative to root.
		};
		
		struct CNotification_Quiescent /// \brief Notification info for #ENotification_Quiescent. \headerfile Mib/Cloud/BackupManagerClient
		{
		};
		
		using CNotification
			= NContainer::TCStreamableVariant
			<
				ENotification
				, CNotification_BackupAborted, ENotification_BackupAborted
				, CNotification_BackupFailed, ENotification_BackupFailed
				, CNotification_FileFinished, ENotification_FileFinished
				, CNotification_Quiescent, ENotification_Quiescent
			>
		; ///< \brief Notification variant. \sa ENotification

		CBackupManagerClient
			(
				CConfig const &_Config
				, NConcurrency::TCActor<NConcurrency::CDistributedActorTrustManager> const &_TrustManager
			)
		; ///< \brief Constructor. \sa CConfig for configuring what to backup and how
		
		~CBackupManagerClient(); ///< \brief Destructor
		
		NConcurrency::TCContinuation<NConcurrency::CActorSubscription> f_SubscribeNotifications /// \brief Subscribe to notification. \sa ENotification
			(
				ENotification _ToSubscribeTo	/// Specify the notifications to subscribe to 
				, NConcurrency::TCActorFunctor<NConcurrency::TCContinuation<void> (NConcurrency::CHostInfo const &_RemoteHost, CNotification &&_Notification)> &&_fOnFinished
				/// The actor functor to receive notifications
			)
		;
		
		void f_StartNewBackup(); ///< Forces a new backup to start outside of the regular scheduling configured through CConfig::m_NewBackupInterval
		
	protected:
		struct CInternal;
		friend struct NPrivate::CBackupManagerClient_Instance;
		
		NConcurrency::TCContinuation<void> fp_Destroy() override;
		
		void fp_OnNotification(NConcurrency::CHostInfo const &_RemoteHost, CNotification &&_Notification);
		
		NPtr::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif
