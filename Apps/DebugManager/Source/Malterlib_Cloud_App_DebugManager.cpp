// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/LogError>
#include <Mib/Daemon/Daemon>

#include "Malterlib_Cloud_App_DebugManager.h"

namespace NMib::NCloud::NDebugManager
{
	CDebugManagerApp::CDebugManagerApp()
		: CDistributedAppActor(CDistributedAppActor_Settings{"DebugManager"})
	{
	}
	
	CDebugManagerApp::~CDebugManagerApp()
	{
	}

	TCFuture<void> CDebugManagerApp::fp_StartApp(NEncoding::CEJsonSorted const _Params)
	{
		co_await fp_SetupPermissions();
		co_await fp_SetupDatabase();
		co_await fp_Publish();

		co_return {};
	}

	TCFuture<void> CDebugManagerApp::fp_StopApp()
	{	
		co_await fp_DestroyAll();

		co_return {};
	}

	TCFuture<void> CDebugManagerApp::fp_Destroy()
	{
		co_await fp_DestroyAll();

		co_await CDistributedAppActor::fp_Destroy();

		co_return {};
	}

	TCFuture<void> CDebugManagerApp::fp_DestroyAll()
	{
		TCFutureVector<void> Destroys;

		CLogError LogError("DebugManager");

		if (mp_PendingNotificationsTimeout)
			fg_Exchange(mp_PendingNotificationsTimeout, nullptr)->f_Destroy() > Destroys;

		for (auto &Downloads : mp_Downloads)
		{
			if (Downloads.m_FileTransferSend)
				fg_Move(Downloads.m_FileTransferSend).f_Destroy() > Destroys;
		}
		mp_Downloads.f_Clear();

		for (auto &Uploads : mp_Uploads)
		{
			if (Uploads.m_FileTransferReceive)
				fg_Move(Uploads.m_FileTransferReceive).f_Destroy() > Destroys;
		}
		mp_Uploads.f_Clear();

		co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy dependencies");

		co_await fp_Notify_Process(true);

		co_await mp_DebugManagerInterface.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy protocol interface");

		co_return {};
	}

	COptionalFuture::COptionalFuture(TCFuture<void> &&_Future)
		: m_Future(fg_Move(_Future))
	{
	}

	COptionalFuture::COptionalFuture(COptionalFuture &&) = default;

	COptionalFuture::~COptionalFuture()
	{
		if (m_Future.f_IsValid())
			m_Future.f_DiscardResult();
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_DebugManager()
	{
		return fg_Construct<NDebugManager::CDebugManagerApp>();
	}
}
