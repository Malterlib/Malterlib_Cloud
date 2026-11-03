// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NCloud
{
	inline fp64 CBackupManagerClient::CFileTransferStats::f_IncomingBytesPerSecond() const
	{
		if (m_nSeconds == 0.0)
			return 0.0;

		return fp64(m_IncomingBytes) / m_nSeconds;
	}

	inline fp64 CBackupManagerClient::CFileTransferStats::f_OutgoingBytesPerSecond() const
	{
		if (m_nSeconds == 0.0)
			return 0.0;

		return fp64(m_OutgoingBytes) / m_nSeconds;
	}

	template <typename tf_CStr>
	void CBackupManagerClient::CFileTransferStats::f_Format(tf_CStr &o_Str) const
	{
		ch8 const *pType = "Unknown";
		switch (m_Type)
		{
		case EFileTransferType_RSync: pType = "RSync"; break;
		case EFileTransferType_Append: pType = "Append"; break;
		case EFileTransferType_Delete: pType = "Delete"; break;
		case EFileTransferType_Rename: pType = "Rename"; break;
		}

		o_Str
			+= typename tf_CStr::CFormat("{} Incoming: {ns } B ({fe2} MB/s) Outgoing: {ns } ({fe2} MB/s)")
			<< pType
			<< m_IncomingBytes
			<< f_IncomingBytesPerSecond() / 1'000'000.0
			<< m_OutgoingBytes
			<< f_OutgoingBytesPerSecond() / 1'000'000.0
		;
	}

	template <typename tf_CStr>
	void CBackupManagerClient::CNotification_BackupAborted::f_Format(tf_CStr &o_String) const
	{
		o_String += "Backup Aborted";
	}

	template <typename tf_CStr>
	void CBackupManagerClient::CNotification_BackupError::f_Format(tf_CStr &o_String) const
	{
		o_String += typename tf_CStr::CFormat("Backup Error{}: {}") << (m_bFatal ? " (Fatal)" : "") << m_ErrorMessage;
	}

	template <typename tf_CStr>
	void CBackupManagerClient::CNotification_FileFinished::f_Format(tf_CStr &o_String) const
	{
		o_String += typename tf_CStr::CFormat("File Finished: {}: {}") << m_FileName << m_TransferStats;
	}

	template <typename tf_CStr>
	void CBackupManagerClient::CNotification_Quiescent::f_Format(tf_CStr &o_String) const
	{
		o_String += "Quiescent";
	}

	template <typename tf_CStr>
	void CBackupManagerClient::CNotification_Unquiescent::f_Format(tf_CStr &o_String) const
	{
		o_String += "Unquiescent";
	}

	template <typename tf_CStr>
	void CBackupManagerClient::CNotification_InitialFinished::f_Format(tf_CStr &o_String) const
	{
		o_String += typename tf_CStr::CFormat("Initial Finished: Added: {vs}   Removed: {vs}  Updated: {vs}") << m_AddedFiles << m_RemovedFiles << m_UpdatedFiles;
	}
}

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif
