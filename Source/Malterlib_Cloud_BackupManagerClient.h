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
#include <Mib/Concurrency/DistributedApp>

namespace NMib::NCloud
{
	namespace NPrivate
	{
		struct CBackupManagerClient_Instance;
	}
	
	/// \brief Implements a client that backs up through backup manager interface
	struct CBackupManagerClient : public NConcurrency::CActor
	{
		using EManifestSyncFlag = NFile::EDirectoryManifestSyncFlag;
		
		struct CConfig /// \brief Config for backup manager client. \headerfile Mib/Cloud/BackupManagerClient
		{
			CConfig const &f_Validate() const; ///< Throws exception if settings are invalid
			
			NStr::CStr m_BackupIdentifier;

			NFile::CDirectoryManifestConfig m_ManifestConfig;										///< The config to generate the manifest
			NTime::CTimeSpan m_NewBackupInterval = NTime::CTimeSpanConvert::fs_CreateDaySpan(1);	///< Interval for creating new full backup snapshots. Set to 0 to disable.
			NStr::CStr m_LogCategory = "Backup";													///< The category to do logging under.
			uint32 m_MaxSendQueue = 8*1024*1024;													///< The maximum number of bytes to queue on the network
			fp32 m_ChangeAggregationTime = 1.0;														///< The number of seconds to aggregate changes over
			bool m_bReportChangesInInitialFinished = false;											///< Include added/removed/updated files in InitialFinished notification
		};
		
		enum ENotification /// Notification from backup manager client
		{
			ENotification_None = 0							///< Used to specify no notification when subscribing. \sa f_SubscribeNotifications
			, ENotification_BackupAborted = DMibBit(0)		///< Backup was aborted remotely. \sa CNotification_BackupAborted
			, ENotification_BackupError = DMibBit(1)		///< Backup failed. \sa CNotification_BackupError
			, ENotification_FileFinished = DMibBit(2)		///< A file finished transferring to backup manager. \sa CNotification_FileFinished
			, ENotification_Quiescent = DMibBit(3)			///< The backup is quiescent. All currently known files have finished transferring. \sa CNotification_Quiescent
			, ENotification_Unquiescent = DMibBit(4)		///< The backup is no longer quiescent. \sa CNotification_Unquiescent
			, ENotification_InitialFinished = DMibBit(5)	///< All files in manifest has been backup up at least once. \sa CNotification_InitialFinished
		};

		enum EFileTransferType
		{
			EFileTransferType_RSync
			, EFileTransferType_Append
			, EFileTransferType_Delete
			, EFileTransferType_Rename
		};

		struct CFileTransferStats
		{
			auto operator <=> (CFileTransferStats const &_Right) const = default;

			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;
			fp64 f_IncomingBytesPerSecond() const;
			fp64 f_OutgoingBytesPerSecond() const;

			uint64 m_OutgoingBytes = 0;
			uint64 m_IncomingBytes = 0;
			fp64 m_nSeconds = 0.0;
			EFileTransferType m_Type = EFileTransferType_RSync;
		};

		struct CNotification_BackupAborted /// \brief Notification info for #ENotification_BackupAborted. \headerfile Mib/Cloud/BackupManagerClient
		{
			auto operator <=> (CNotification_BackupAborted const &_Right) const = default;

			template <typename tf_CStr>
			void f_Format(tf_CStr &o_String) const
			{
				o_String += "Backup Aborted";
			}
		};
		
		struct CNotification_BackupError /// \brief Notification info for #ENotification_BackupError. \headerfile Mib/Cloud/BackupManagerClient
		{
			auto operator <=> (CNotification_BackupError const &_Right) const = default;

			template <typename tf_CStr>
			void f_Format(tf_CStr& o_String) const
			{
				o_String += typename tf_CStr::CFormat("Backup Error{}: {}") << (m_bFatal ? " (Fatal)" : "") << m_ErrorMessage;
			}

			NStr::CStr m_ErrorMessage; ///< The error from exception that caused the backup to fail
			bool m_bFatal = false; ///< If this is set to true the backup is in a fatal state, and cannot recover automatically.
		};
		
		struct CNotification_FileFinished /// \brief Notification info for #ENotification_FileFinished. \headerfile Mib/Cloud/BackupManagerClient
		{
			auto operator <=> (CNotification_FileFinished const &_Right) const = default;

			template <typename tf_CStr>
			void f_Format(tf_CStr& o_String) const
			{
				o_String += typename tf_CStr::CFormat("File Finished: {}: {}") << m_FileName << m_TransferStats;
			}

			NStr::CStr m_FileName; ///< The file that finished backing up. Relative to root.
			CFileTransferStats m_TransferStats; ///< Statistics for the file transfer;
		};
		
		struct CNotification_Quiescent /// \brief Notification info for #ENotification_Quiescent. \headerfile Mib/Cloud/BackupManagerClient
		{
			auto operator <=> (CNotification_Quiescent const &_Right) const = default;

			template <typename tf_CStr>
			void f_Format(tf_CStr &o_String) const
			{
				o_String += "Quiescent";
			}
		};

		struct CNotification_Unquiescent /// \brief Notification info for #ENotification_Unquiescent. \headerfile Mib/Cloud/BackupManagerClient
		{
			auto operator <=> (CNotification_Unquiescent const &_Right) const = default;

			template <typename tf_CStr>
			void f_Format(tf_CStr& o_String) const
			{
				o_String += "Unquiescent";
			}
		};

		struct CNotification_InitialFinished /// \brief Notification info for #ENotification_InitialFinished. \headerfile Mib/Cloud/BackupManagerClient
		{
			auto operator <=> (CNotification_InitialFinished const &_Right) const = default;

			template <typename tf_CStr>
			void f_Format(tf_CStr& o_String) const
			{
				o_String += typename tf_CStr::CFormat("Initial Finished: Added: {vs}   Removed: {vs}  Updated: {vs}") << m_AddedFiles << m_RemovedFiles << m_UpdatedFiles;
			}

			NContainer::TCVector<NStr::CStr> m_AddedFiles;
			NContainer::TCVector<NStr::CStr> m_RemovedFiles;
			NContainer::TCVector<NStr::CStr> m_UpdatedFiles;
		};
		
		using CNotification
			= NStorage::TCStreamableVariant
			<
				ENotification
				, NStorage::TCMember<CNotification_BackupAborted, ENotification_BackupAborted>
				, NStorage::TCMember<CNotification_BackupError, ENotification_BackupError>
				, NStorage::TCMember<CNotification_FileFinished, ENotification_FileFinished>
				, NStorage::TCMember<CNotification_Quiescent, ENotification_Quiescent>
				, NStorage::TCMember<CNotification_Unquiescent, ENotification_Unquiescent>
				, NStorage::TCMember<CNotification_InitialFinished, ENotification_InitialFinished>
			>
		; ///< \brief Notification variant. \sa ENotification

		CBackupManagerClient
			(
				CConfig const &_Config
				, NConcurrency::TCActor<NConcurrency::CDistributedActorTrustManager> const &_TrustManager
				, NConcurrency::TCActorFunctor
				<
					NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>>
					(
						NConcurrency::TCDistributedActorInterfaceWithID<NConcurrency::CDistributedAppInterfaceBackup> &&_BackupInterface
						, NConcurrency::CActorSubscription &&_ManifestFinished
						, NStr::CStr const &_BackupRoot
					)
				>
				&&_fOnNewBackup = {}
				, NConcurrency::TCActor<NConcurrency::CActorDistributionManager> const &_DistributionManager = {}
			)
		; ///< \brief Constructor. \sa CConfig for configuring what to backup and how

		~CBackupManagerClient(); ///< \brief Destructor

		NConcurrency::TCFuture<void> f_StartBackup(); ///< Start backup. Can only be called once. If you need to subscribe to notifications you should do this before starting backup.

		NConcurrency::TCFuture<NConcurrency::CActorSubscription> f_SubscribeNotifications /// \brief Subscribe to notification. \sa ENotification
			(
				ENotification _ToSubscribeTo	/// Specify the notifications to subscribe to 
				, NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NConcurrency::CHostInfo const &_RemoteHost, CNotification &&_Notification)> &&_fOnNotification
				/// The actor functor to receive notifications
			)
		;

		NConcurrency::TCFuture<void> f_StartNewBackup(); ///< Forces a new backup to start outside of the regular scheduling configured through CConfig::m_NewBackupInterval

	protected:
		struct CInternal;
		friend struct NPrivate::CBackupManagerClient_Instance;
		
		NConcurrency::TCFuture<void> fp_Destroy() override;
		
		void fp_OnNotification(NConcurrency::CHostInfo const &_RemoteHost, CNotification &&_Notification);
		void fp_HashMismatch(NStr::CStr const &_File);

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_BackupManagerClient.hpp"
