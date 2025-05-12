// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_DebugManager.h"

namespace NMib::NCloud::NDebugManager
{
	TCFuture<void> CDebugManagerApp::fp_Publish()
	{
		co_await mp_DebugManagerInterface.f_Publish<CDebugManager>(mp_State.m_DistributionManager, this);

		co_return {};
	}

	CDebugManagerApp::CDownload::CDownload() = default;

	CDebugManagerApp::CDownload::~CDownload()
	{
		if (m_FileTransferSend)
			fg_Move(m_FileTransferSend).f_Destroy() > fg_LogWarning("Download", "Failed to destroy file transfer send in destructor");
	}

	CDebugManagerApp::CUpload::CUpload() = default;

	CDebugManagerApp::CUpload::~CUpload()
	{
		if (m_FileTransferReceive)
			fg_Move(m_FileTransferReceive).f_Destroy() > fg_LogWarning("Upload", "Failed to destroy file transfer receive in destructor");
	}
}
