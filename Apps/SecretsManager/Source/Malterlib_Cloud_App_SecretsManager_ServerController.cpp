// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager_ServerController.h"

namespace NMib::NCloud::NSecretsManager
{
	CSecretsManagerDaemonActor::CServerController::CServerController(TCActor<> const &_Delegator, CDistributedAppState &_AppState)
		: mp_ServerActor(nullptr)
		, mp_AppState(_AppState)
		, mp_Delegator(_Delegator)
	{
		fp_Init();
	}

	CSecretsManagerDaemonActor::CServerController::~CServerController()
	{
	}

#if DMibConfig_Tests_Enable
	TCFuture<CEJSON> CSecretsManagerDaemonActor::CServerController::f_Test_Command(CStr const &_Command, CEJSON const &_Params)
	{
		if (!mp_ServerActor)
			co_return DMibErrorInstance("No server");

		co_return co_await mp_ServerActor.f_CallActor(&CSecretsManagerDaemonActor::CServer::f_Test_Command)(_Command, _Params);
	}
#endif

	void CSecretsManagerDaemonActor::CServerController::fp_Init()
	{
		DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "ServerController started");

		mp_AppState.m_TrustManager
			(
				&CDistributedActorTrustManager::f_SubscribeTrustedActors<CKeyManager>
				, "com.malterlib/Cloud/KeyManager"
				, fg_ThisActor(this)
			)
			> [this](TCAsyncResult<TCTrustedActorSubscription<CKeyManager>> &&_KeySubscription)
			{
				if (mp_AppState.m_bStoppingApp)
					return;

				if (!_KeySubscription)
				{
					DMibLogWithCategory(Mib/Cloud/SecretsManager, Error, "Failed to subscribe to KeyManagers, aborting startup: {}", _KeySubscription.f_GetExceptionStr());
					return;
				}
				else
					DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "Acquired KeyManager subscription");

				mp_KeyManagerSubscription = fg_Move(*_KeySubscription);
				mp_KeyManagerSubscription.f_OnActor
					(
						[this](TCDistributedActor<CKeyManager> const &_KeyManager, CTrustedActorInfo const &_ActorInfo)
						{
							self(&CServerController::fp_KeyManagerAvailable, _KeyManager) > fg_DiscardResult();
						}
					)
				;
			}
		;
	}

	TCFuture<void> CSecretsManagerDaemonActor::CServerController::fp_KeyManagerAvailable(TCDistributedActor<CKeyManager> const &_KeyManager)
	{
		if (mp_ServerActor || mp_AppState.m_bStoppingApp)
			co_return {};

		static const mint c_KeyBits = 512;
		CSymmetricKey Key = co_await _KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("SecretsManagerDB", c_KeyBits / 8);

		if (mp_ServerActor || mp_AppState.m_bStoppingApp)
			co_return {};

		DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "Keymanager available, reading database");

		CStr DatabasePath = mp_AppState.m_RootDirectory + "/SecretsManagerDB.enc";
		auto DatabaseActor = fg_ConstructActor<CSecretsManagerServerDatabase>(fg_Construct("SecretsManager Database"), DatabasePath, fg_Move(Key));

		mp_PendingDatabases[DatabaseActor];

		auto Cleanup = g_OnScopeExitActor > [this, DatabaseActorWeak = DatabaseActor.f_Weak()]() -> TCFuture<void>
			{
				auto DatabaseActor = DatabaseActorWeak.f_Lock();

				if (!DatabaseActor || !mp_PendingDatabases.f_FindEqual(DatabaseActorWeak))
				{
					mp_PendingDatabases.f_Remove(DatabaseActorWeak);
					co_return {};
				}

				co_await fg_Move(DatabaseActor).f_Destroy().f_Wrap();
				mp_PendingDatabases.f_Remove(DatabaseActorWeak);
				co_return {};
			}
		;

		auto DatabaseActorWeak = DatabaseActor.f_Weak();
		auto Result = co_await fg_Move(DatabaseActor)(&CSecretsManagerServerDatabase::f_Initialize).f_Wrap();

		DMibFastCheck(!DatabaseActor);

		if (!Result)
		{
			DMibLogWithCategory(Mib/Cloud/SecretsManager, Error, "Failed to read database: {}", Result.f_GetExceptionStr());
			co_return {};
		}

		if (mp_ServerActor || mp_AppState.m_bStoppingApp)
			co_return {};

		DatabaseActor = DatabaseActorWeak.f_Lock();
		if (!DatabaseActor)
			co_return {};

		DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "Database initialized, starting server");

		mp_PendingDatabases.f_Remove(DatabaseActorWeak);
		mp_ServerActor = fg_ConstructActor<CSecretsManagerDaemonActor::CServer>(fg_Construct(mp_Delegator), mp_AppState, fg_Move(DatabaseActor));

		co_return {};
	}

	TCFuture<void> CSecretsManagerDaemonActor::CServerController::fp_Destroy()
	{
		TCActorResultVector<void> Destroys;

		if (mp_ServerActor)
			mp_ServerActor.f_Destroy() > Destroys.f_AddResult();

		for (auto Database : mp_PendingDatabases)
			fg_Move(Database).f_Destroy() > Destroys.f_AddResult();
		mp_PendingDatabases.f_Clear();

		co_await Destroys.f_GetResults();

		co_return {};
	}
}
