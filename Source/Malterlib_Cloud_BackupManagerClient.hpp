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

		o_Str += typename tf_CStr::CFormat("{} Incoming: {ns } B ({fe2} MB/s) Outgoing: {ns } ({fe2} MB/s)")
			<< pType
			<< m_IncomingBytes
			<< f_IncomingBytesPerSecond()/1'000'000.0
			<< m_OutgoingBytes
			<< f_OutgoingBytesPerSecond()/1'000'000.0
		;
	}
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
