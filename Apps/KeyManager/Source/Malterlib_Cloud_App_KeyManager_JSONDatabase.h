// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/EJSON>
#include <Mib/Concurrency/ConcurrencyDefines>

namespace NMib
{
	namespace NCloud
	{
		namespace NKeyManager
		{
			struct CJSONDatabase
			{
			public:
				CJSONDatabase(NStr::CStr const &_FileName);
				
				TCDispatchedActorCall<void> f_Load();
				TCDispatchedActorCall<void> f_Save();
				
				CEJSON m_Data;
				
			private:
				TCActor<CSeparateThreadActor> mp_FileActor;
				NStr::CStr mp_FileName;
			};
		}		
	}
}
