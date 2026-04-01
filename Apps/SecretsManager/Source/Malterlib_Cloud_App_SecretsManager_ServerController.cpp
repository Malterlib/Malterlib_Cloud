// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Cloud_App_SecretsManager_ServerController.h"

namespace NMib::NCloud::NSecretsManager
{
	CSecretsManagerDaemonActor::CServerController::CServerController(TCActor<> const &_Delegator, CDistributedAppState &_AppState)
		: mp_ServerActor(nullptr)
		, mp_AppState(_AppState)
		, mp_Delegator(_Delegator)
	{
	}

	CSecretsManagerDaemonActor::CServerController::~CServerController()
	{
	}

#if DMibConfig_Tests_Enable
	TCFuture<CEJsonSorted> CSecretsManagerDaemonActor::CServerController::f_Test_Command(CStr _Command, CEJsonSorted const _Params)
	{
		if (!mp_ServerActor)
			co_return DMibErrorInstance("No server");

		co_return co_await mp_ServerActor.f_CallActor(&CSecretsManagerDaemonActor::CServer::f_Test_Command)(fg_Move(_Command), fg_Move(fg_RemoveQualifiers(_Params)));
	}
#endif

	TCFuture<void> CSecretsManagerDaemonActor::CServerController::f_Init()
	{
		DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "ServerController started");

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					if (mp_AppState.m_bStoppingApp || f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto KeyManagerSubscription = co_await mp_AppState.m_TrustManager->f_SubscribeTrustedActors<CKeyManager>().f_Wrap();

		if (!KeyManagerSubscription)
		{
			DMibLogWithCategory(Mib/Cloud/SecretsManager, Error, "Failed to subscribe to KeyManagers, aborting startup: {}", KeyManagerSubscription.f_GetExceptionStr());
			co_return {};
		}
		else
			DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "Acquired KeyManager subscription");

		mp_KeyManagerSubscription = fg_Move(*KeyManagerSubscription);
		co_await mp_KeyManagerSubscription.f_OnActor
			(
				g_ActorFunctor / [this](TCDistributedActor<CKeyManager> _KeyManager, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
				{
					co_await fp_KeyManagerAvailable(_KeyManager);
					co_return {};
				}
				, nullptr
			)
		;

		co_return {};
	}

	TCFuture<void> CSecretsManagerDaemonActor::CServerController::fp_KeyManagerAvailable(TCDistributedActor<CKeyManager> _KeyManager)
	{
		if (mp_ServerActor)
			co_return {};

		auto CheckDestroy = co_await fg_OnResume
			(
				[this]() -> TCAsyncResult<void>
				{
					TCAsyncResult<void> Return;

					if (f_IsDestroyed() || mp_AppState.m_bStoppingApp)
						Return.f_SetException(DMibImpExceptionInstance(CExceptionActorIsBeingDestroyed, "Shutting down"));
					else if (mp_ServerActor)
						Return.f_SetResult();

					return Return;
				}
			)
		;

		static const umint c_KeyBits = 512;
		CSymmetricKey Key = co_await _KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("SecretsManagerDB", c_KeyBits / 8);

		DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "Keymanager available, reading database");

		CStr DatabasePath = mp_AppState.m_RootDirectory + "/SecretsManagerDB.enc";
		auto DatabaseActor = fg_ConstructActor<CSecretsManagerServerDatabase>(DatabasePath, fg_Move(Key));

		mp_PendingDatabases[DatabaseActor];

		auto Cleanup = g_OnScopeExitActor / [this, DatabaseActorWeak = DatabaseActor.f_Weak()]() -> TCFuture<void>
			{
				auto DatabaseActor = DatabaseActorWeak.f_Lock();

				if (!DatabaseActor || !mp_PendingDatabases.f_FindEqual(DatabaseActorWeak))
				{
					mp_PendingDatabases.f_Remove(DatabaseActorWeak);
					co_return {};
				}

				co_await fg_Move(DatabaseActor).f_Destroy().f_Wrap() > fg_LogWarning("Mib/Cloud/SecretsManager", "Failed to destroy database actor");
				mp_PendingDatabases.f_Remove(DatabaseActorWeak);
				co_return {};
			}
		;

		// Use sequencer to prevent race condition where multiple callbacks try to initialize the database simultaneously
		auto SequenceSubscription = co_await mp_InitSequencer.f_Sequence();

		auto DatabaseActorWeak = DatabaseActor.f_Weak();

		auto Result = co_await fg_Move(DatabaseActor)(&CSecretsManagerServerDatabase::f_Initialize).f_Wrap();

		DMibFastCheck(!DatabaseActor);

		if (!Result)
		{
			DMibLogWithCategory(Mib/Cloud/SecretsManager, Error, "Failed to read database: {}", Result.f_GetExceptionStr());
			co_return {};
		}

		DatabaseActor = DatabaseActorWeak.f_Lock();
		if (!DatabaseActor)
			co_return {};

		DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "Database initialized, starting server");

		mp_PendingDatabases.f_Remove(DatabaseActorWeak);
		mp_ServerActor = fg_ConstructActor<CSecretsManagerDaemonActor::CServer>(fg_Construct(mp_Delegator), mp_AppState, fg_Move(DatabaseActor));

		co_await mp_ServerActor(&CSecretsManagerDaemonActor::CServer::f_Init);

		co_return {};
	}

	TCFuture<void> CSecretsManagerDaemonActor::CServerController::fp_Destroy()
	{
		TCFutureVector<void> Destroys;

		CLogError LogError("Mib/Cloud/SecretsManager");

		if (mp_ServerActor)
			fg_TempCopy(mp_ServerActor).f_Destroy() > Destroys;

		for (auto Database : mp_PendingDatabases)
			fg_Move(Database).f_Destroy() > Destroys;
		mp_PendingDatabases.f_Clear();

		co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy server actor or databases");

		co_await mp_KeyManagerSubscription.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy key manager subscription");

		co_await fg_Move(mp_InitSequencer).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy init sequencer");

		co_return {};
	}
}
