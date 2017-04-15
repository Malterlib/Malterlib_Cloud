// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_DirectorySync.h"

namespace NMib::NCloud
{
	CDirectorySyncClient::CDirectorySyncClient()
	{
		DMibPublishActorFunction(CDirectorySyncClient::f_StartManifestRSync);
		DMibPublishActorFunction(CDirectorySyncClient::f_StartRSync);
		DMibPublishActorFunction(CDirectorySyncClient::f_Finished);
	}
	
	fp64 CDirectorySyncStats::f_IncomingBytesPerSecond() const
	{
		if (m_nSeconds == 0.0)
			return 0.0;

		return fp64(m_IncomingBytes) / m_nSeconds;
	}
	
	fp64 CDirectorySyncStats::f_OutgoingBytesPerSecond() const
	{
		if (m_nSeconds == 0.0)
			return 0.0;

		return fp64(m_OutgoingBytes) / m_nSeconds;
	}
}
