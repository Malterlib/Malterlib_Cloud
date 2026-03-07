// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_SecretsManagerUpload.h"
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NStorage;
	using namespace NFile;
	using namespace NStr;

	namespace
	{
		struct CState
		{
			template <typename tf_CResult>
			void f_SetException(tf_CResult &&_Result)
			{
				m_Promise.f_SetException(fg_Forward<tf_CResult>(_Result));
				m_bSeenException = true;
			}

			void f_SetFunctor(TCActorFunctorWithID<TCFuture<void> ()> &&_fFunctor)
			{
				if (m_bSeenException)
					return;

				m_fFunctor = fg_Move(_fFunctor);
				fp_CallFunctor();
			}

			void f_SetResult(CDirectorySyncSend::CSyncResult &&_SyncResult)
			{
				if (m_bSeenException)
					return;

				m_SyncResults = fg_Move(_SyncResult);
				m_bSeenResult = true;
				fp_CallFunctor();
			}

			TCPromise<CDirectorySyncSend::CSyncResult> m_Promise;
			TCActorFunctorWithID<TCFuture<void> ()> m_fFunctor;
			TCDistributedActor<CDirectorySyncSend> m_DirectorySyncSend;
			CDirectorySyncSend::CSyncResult m_SyncResults;
			bool m_bSeenResult = false;
			bool m_bSeenException = false;
			bool m_bAborted = false;

		private:
			void fp_CallFunctor()
			{
				if (m_bSeenResult && m_fFunctor)
				{
					m_fFunctor() > [Promise = m_Promise, SyncResults = m_SyncResults](TCAsyncResult<void> &&_Result)
						{
							Promise.f_SetExceptionOrResult(_Result, SyncResults);
						}
					;
				}
			}
		};
	}

	TCFuture<CDirectorySyncSend::CSyncResult> DMibWorkaroundUBSanSectionErrors fg_UploadSecretFile
		(
			TCDistributedActor<CSecretsManager> _SecretsManager
			, TCActor<CActorDistributionManager> _DistributionManager
			, CSecretsManager::CSecretID _ID
			, CDirectorySyncSend::CConfig _Config
			, NReference::TCReference<CActorSubscription> o_Subscription
		)
	{
		NStorage::TCSharedPointer<CState> pState = fg_Construct();

		if (!_Config.m_Manifest.f_IsOfType<CDirectoryManifestConfig>() || _Config.m_Manifest.f_Get<1>().m_IncludeWildcards.f_GetLen() != 1)
			co_return DMibErrorInstance("Incorrect config. Expected a CDirectoryManifestConfig with a single file in m_IncludeWildcards");

		o_Subscription.f_Get() = g_ActorSubscription / [pState]() -> TCFuture<void>
			{
				pState->m_bAborted = true;

				if (pState->m_DirectorySyncSend)
					co_await fg_Move(pState->m_DirectorySyncSend).f_Destroy();

				co_return {};
			}
		;

		CStr const FileName = *_Config.m_Manifest.f_Get<1>().m_IncludeWildcards.f_FindSmallestKey();
		pState->m_DirectorySyncSend = _DistributionManager->f_ConstructActor<CDirectorySyncSend>(fg_Move(_Config));

		TCDistributedActorInterfaceWithID<CDirectorySyncClient> SyncInterface
			{
				pState->m_DirectorySyncSend->f_ShareInterface<CDirectorySyncClient>()
				, g_ActorSubscription / [pState]() mutable -> TCFuture<void>
				{
					if (pState->m_bAborted || !pState->m_DirectorySyncSend)
						co_return DMibErrorInstance("Aborted");

					TCAsyncResult<CDirectorySyncSend::CSyncResult> Result = co_await pState->m_DirectorySyncSend.f_CallActor(&CDirectorySyncSend::f_GetResult)().f_Wrap();

					if (!Result)
						pState->f_SetException(Result);
					else
						pState->f_SetResult(fg_Move(*Result));

					if (pState->m_DirectorySyncSend && !pState->m_bAborted)
					{
						pState->m_bAborted = true;
						co_await fg_Move(pState->m_DirectorySyncSend).f_Destroy();
					}

					co_return {};
				}
			}
		;

		auto Result = co_await _SecretsManager.f_CallActor(&CSecretsManager::f_UploadFile)(fg_Move(_ID), FileName, fg_Move(SyncInterface)).f_Wrap();

		if (!Result)
			pState->f_SetException(Result);
		else
			pState->f_SetFunctor(fg_Move(*Result));

		co_return co_await pState->m_Promise.f_Future();
	}
}
