// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_KeyManager.h"
#include "Malterlib_Cloud_KeyManagerServer.h"
#include "Malterlib_Cloud_KeyManagerServer_Internal.h"

namespace NMib
{
	namespace NCloud
	{
		using namespace NConcurrency;
		using namespace NConcurrency::NPrivate;
		
		CKeyManagerServer::CInternal::CInternal(CKeyManagerServer *_pThis, CKeyManagerServerConfig const &_Config)
			: m_pThis(_pThis)
			, m_Config(_Config)
			, m_pDatabase(nullptr)
		{
			
		}
			
		void CKeyManagerServer::CInternal::CInternal::f_ReadDatabase(NFunction::TCFunction<void ()> &&_fOnReady, NFunction::TCFunction<void (NStr::CStr const&)> &&_fOnError)
		{
			if (m_pDatabase)
			{
				_fOnReady();
				return;
			}
			
			m_OnDatabaseReadyQueue.f_Insert(fg_Move(_fOnReady));
			m_OnDatabaseErrorQueue.f_Insert(fg_Move(_fOnError));
			
			m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase) > [this] (NConcurrency::TCAsyncResult<ICKeyManagerServerDatabase::CDatabase> &&_Database)
				{
					if (!_Database)
					{
						DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Error, "Failed to read database: {}", _Database.f_GetExceptionStr());
						for (auto &fCallWithError : m_OnDatabaseErrorQueue)
							fCallWithError(_Database.f_GetExceptionStr());
						m_OnDatabaseErrorQueue.f_Clear();
						m_OnDatabaseReadyQueue.f_Clear();
						return;
					}
					
					m_pDatabase = fg_Construct(*_Database);
					for (auto &fCall : m_OnDatabaseReadyQueue)
						fCall();
					
					m_OnDatabaseReadyQueue.f_Clear();
					m_OnDatabaseErrorQueue.f_Clear();
				}
			;
		}
			
		NConcurrency::TCContinuation<CSymmetricKey> CKeyManagerServer::CInternal::CInternal::f_RequestKey(NStr::CStr const &_HostID, NStr::CStr const &_Identifier, uint32 _KeySize)
		{
			NConcurrency::TCContinuation<CSymmetricKey> Continuation;
			
			f_ReadDatabase
				(
					[this, Continuation, _HostID, _Identifier, _KeySize]
					{
						auto &Client = m_pDatabase->m_Clients[_HostID];
						auto pKey = Client.m_Keys.f_FindEqual(_Identifier);
						if (!pKey)
						{
							pKey = &Client.m_Keys[_Identifier];
							pKey->f_SetLen(_KeySize);
							NSys::fg_Security_GenerateHighEntropyData(pKey->f_GetArray(), pKey->f_GetLen());
							
							m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, *m_pDatabase)
								> [Continuation, Key = *pKey] (NConcurrency::TCAsyncResult<void> &&_Result) mutable
								{
									if (!_Result)
									{
										DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Error, "Failed to write database: {}", _Result.f_GetExceptionStr());
										Continuation.f_SetException(DMibErrorInstance(fg_Format("Failed to write database: {}", _Result.f_GetExceptionStr())));
										return;
									}
									
									Continuation.f_SetResult(fg_Move(Key));
								}
							;
						}
						else
							Continuation.f_SetResult(*pKey);
					}
					, [Continuation] (NStr::CStr const &_Error)
					{
						Continuation.f_SetException(DMibErrorInstance(fg_Format("Failed to read database: {}", _Error)));
					}
				)
			;
			return Continuation;
		}		
		
		CKeyManagerServer::CKeyManagerServer(CKeyManagerServerConfig const &_Config)
			: mp_pInternal(fg_Construct(this, _Config))
		{
			
		}
		
		CKeyManagerServer::~CKeyManagerServer()
		{
		}
		
		void CKeyManagerServer::f_Construct()
		{
			auto &Internal = *mp_pInternal;
			Internal.m_KeyManagerActor = fg_ConstructDistributedActor<CKeyManager>(fg_ThisActor(this));
			
			auto &DistributionManager = NConcurrency::fg_GetDistributionManager();
			DistributionManager
				(
					&CActorDistributionManager::f_PublishActor
					, Internal.m_KeyManagerActor
					, "MalterlibCloudKeyManager"
					, NConcurrency::CDistributedActorInheritanceHeirarchyPublish::fs_GetHierarchy<CKeyManager>()
				)
				> [this] (TCAsyncResult<CDistributedActorPublication> &&_Publication)
				{
					auto &Internal = *mp_pInternal;
					Internal.m_KeyManagerPublication = fg_Move(*_Publication);
				}
			;
		}
		
		NConcurrency::TCContinuation<CSymmetricKey> CKeyManagerServer::fp_RequestKey(NStr::CStr const &_HostID, NStr::CStr const &_Identifier, uint32 _KeySize)
		{
			return mp_pInternal->f_RequestKey(_HostID, _Identifier, _KeySize);
		}
	}
}
