// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_KeyManager_JSONDatabase.h"
#include <Mib/Concurrency/ConcurrencyManager>

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			CJSONDatabase::CJSONDatabase(NStr::CStr const &_FileName)
				: mp_FileActor(fg_ConstructActor<CSeparateThreadActor>(fg_Construct("JSONDatabase")))
				, mp_FileName(_FileName)
			{
			}
				
			TCDispatchedActorCall<void> CJSONDatabase::f_Load()
			{
				return fg_Dispatch
					(
						[this, FileName = mp_FileName, FileActor = mp_FileActor]
						{
							TCContinuation<void> Continuation;
							fg_Dispatch
								(
									FileActor
									, [FileName]
									{
										if (!CFile::fs_FileExists(FileName))
											return CEJSON();
										return CEJSON::fs_FromString(CFile::fs_ReadStringFromFile(FileName), FileName);
									}
								)
								> Continuation / [this, Continuation](CEJSON &&_Data)
								{
									m_Data = fg_Move(_Data);
									Continuation.f_SetResult();
								}
							;
							return Continuation;
						}
					)
				;
			}
			
			TCDispatchedActorCall<void> CJSONDatabase::f_Save()
			{
				return fg_Dispatch
					(
						mp_FileActor
						, [this, FileName = mp_FileName, Data = m_Data]
						{
							CFile::fs_CreateDirectory(CFile::fs_GetPath(FileName));
							EFileAttrib FileAttributes = EFileAttrib_UnixAttributesValid | EFileAttrib_UserRead | EFileAttrib_UserWrite;
							CFile::fs_WriteStringToFile(FileName, Data.f_ToString(), true, FileAttributes);
						}
					)
				;
			}
		}		
	}
}
