// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_KeyManager.h"
#include "Malterlib_Cloud_KeyManagerServer.h"
#include "Malterlib_Cloud_KeyManagerServer_Internal.h"

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NConcurrency::NPrivate;
	
	CKeyManagerServer::CInternal::CInternal(CKeyManagerServer *_pThis, CKeyManagerServerConfig const &_Config)
		: m_pThis(_pThis)
		, m_Config(_Config)
		, m_pDatabase(nullptr)
	{
		
	}
		
	void CKeyManagerServer::CInternal::f_ReadDatabase(NFunction::TCFunction<void ()> &&_fOnReady, NFunction::TCFunction<void (NStr::CStr const&)> &&_fOnError)
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
	
	NConcurrency::TCContinuation<void> CKeyManagerServer::CInternal::f_PreCreateKeys(uint32 _KeySize, uint32 _nKeys)
	{
		NConcurrency::TCContinuation<void> Continuation;
		
		f_ReadDatabase
			(
				[this, Continuation, _KeySize, _nKeys]
				{
					auto const& CurrentKeys = m_pDatabase->m_AvailableKeys[_KeySize];
					if (CurrentKeys.f_GetLen() >= _nKeys)
					{
						Continuation.f_SetResult();
						return;
					}
					
					NContainer::TCVector<CSymmetricKey> GeneratedKeys;
					GeneratedKeys.f_SetLen(_nKeys - CurrentKeys.f_GetLen());
					
					for (auto& Key : GeneratedKeys)
					{
						Key.f_SetLen(_KeySize);
						NSys::fg_Security_GenerateHighEntropyData(Key.f_GetArray(), Key.f_GetLen());
					}
					
					m_pDatabase->m_AvailableKeys[_KeySize].f_InsertFirst(fg_Move(GeneratedKeys));
					
					m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, *m_pDatabase)
						> [Continuation] (NConcurrency::TCAsyncResult<void> &&_Result) mutable
						{
							if (!_Result)
							{
								DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Error, "Failed to write database: {}", _Result.f_GetExceptionStr());
								Continuation.f_SetException(DMibErrorInstance(fg_Format("Failed to write database: {}", _Result.f_GetExceptionStr())));
								return;
							}
							
							Continuation.f_SetResult();
						}
					;
				}
				, [Continuation] (NStr::CStr const &_Error)
				{
					Continuation.f_SetException(DMibErrorInstance(fg_Format("Failed to read database: {}", _Error)));
				}
			)
		;
		return Continuation;
	}
		
	NConcurrency::TCContinuation<CSymmetricKey> CKeyManagerServer::CInternal::f_RequestKey(NStr::CStr const &_HostID, NStr::CStr const &_Identifier, uint32 _KeySize)
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
						
						auto pAvailableKeys = m_pDatabase->m_AvailableKeys.f_FindEqual(_KeySize);
						if (!pAvailableKeys)
						{
							pKey->f_SetLen(_KeySize);
							NSys::fg_Security_GenerateHighEntropyData(pKey->f_GetArray(), pKey->f_GetLen());
						}
						else
						{
							*pKey = fg_Move(pAvailableKeys->f_GetLast());
							if (pKey->f_GetLen() != _KeySize)
							{
								NStr::CStr Error = NStr::fg_Format("Requested key size mismatch with pre-created key. Requested: {}, Pre-created: {}", _KeySize, pKey->f_GetLen());
								DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Error, "{}", Error);
								Continuation.f_SetException(DMibErrorInstance(Error));
								return;
							}
							
							pAvailableKeys->f_Remove(pAvailableKeys->f_GetLen() - 1);
							if (pAvailableKeys->f_IsEmpty())
								m_pDatabase->m_AvailableKeys.f_Remove(_KeySize);
						}
						
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
					{
						if (pKey->f_GetLen() != _KeySize)
						{
							Continuation.f_SetException(DMibErrorInstance(NStr::fg_Format("Saved key has different size {} from requested size {}", pKey->f_GetLen(), _KeySize)));
							return;
						}
						Continuation.f_SetResult(*pKey);
					}
				}
				, [Continuation] (NStr::CStr const &_Error)
				{
					Continuation.f_SetException(DMibErrorInstance(NStr::fg_Format("Failed to read database: {}", _Error)));
				}
			)
		;
		return Continuation;
	}		
	
	CKeyManagerServer::CKeyManagerServer(CKeyManagerServerConfig const &_Config)
		: mp_pInternal(fg_Construct(this, _Config))
	{
		auto &Internal = *mp_pInternal;
		auto &DistributionManager = NConcurrency::fg_GetDistributionManager();
		Internal.m_KeyManagerActor = DistributionManager->f_ConstructActor<CKeyManager>(fg_ThisActor(this));
		
		Internal.m_KeyManagerActor->f_Publish<CKeyManager>("com.malterlib/Cloud/KeyManager")
			> [this] (TCAsyncResult<CDistributedActorPublication> &&_Publication)
			{
				auto &Internal = *mp_pInternal;
				Internal.m_KeyManagerPublication = fg_Move(*_Publication);
			}
		;
	}
	
	CKeyManagerServer::~CKeyManagerServer()
	{
	}
	
	NConcurrency::TCContinuation<void> CKeyManagerServer::f_PreCreateKeys(uint32 _KeySize, uint32 _nKeys)
	{
		return mp_pInternal->f_PreCreateKeys(_KeySize, _nKeys);
	}
	
	NConcurrency::TCContinuation<CSymmetricKey> CKeyManagerServer::fp_RequestKey(NStr::CStr const &_HostID, NStr::CStr const &_Identifier, uint32 _KeySize)
	{
		return mp_pInternal->f_RequestKey(_HostID, _Identifier, _KeySize);
	}
}
