// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/UUID>

#include "Malterlib_Cloud_App_DebugManager.h"
#include "Malterlib_Cloud_App_DebugManager_Protocol_Conversion.hpp"

namespace NMib::NCloud::NDebugManager
{
	auto CDebugManagerApp::CDebugManagerImplementation::f_Asset_List(CAssetList _Params) -> TCFuture<CAssetList::CResult>
	{
		auto pThis = m_pThis;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();
		auto Auditor = pThis->mp_State.f_Auditor({}, CallingHostInfo);

		TCVector<CStr> Permissions{"DebugManager/ReadAll", "DebugManager/ReadAsset", "DebugManager/ListAll", "DebugManager/ListAsset"};

		auto bHasPermissions = co_await (pThis->mp_Permissions.f_HasPermission("List assets", Permissions) % "Permission denied listing assets" % Auditor);
		if (!bHasPermissions)
			co_return Auditor.f_AccessDenied("(Upload asset)", Permissions);

		auto AssetGenerator = co_await pThis->mp_DebugDatabase
			(
				&CDebugDatabase::f_Asset_List
				, fg_ConvertToDebugDatabase<CDebugDatabase::CAssetFilter>(fg_Move(_Params.m_Filter))
			)
		;

		co_return CAssetList::CResult
			{
				.m_AssetsGenerator = fg_CallSafe
				(
					[AssetGenerator = fg_Move(AssetGenerator), Auditor]() mutable -> NConcurrency::TCAsyncGenerator<NContainer::TCVector<CAssetList::CAsset>>
					{
						umint nAssets = 0;
						bool bDone = false;

						auto Cleanup = g_OnScopeExit / [&]
							{
								Auditor.f_Info("Listed {} assets{}"_f << nAssets << (bDone ? "" : " (aborted)"));
							}
						;

						for (auto iAsset = co_await fg_Move(AssetGenerator).f_GetPipelinedIterator(); iAsset; co_await ++iAsset)
						{
							NContainer::TCVector<CAssetList::CAsset> Assets;
							for (auto &SourceAsset : *iAsset)
								Assets.f_Insert(fg_ConvertToDebugDatabase<CAssetList::CAsset>(fg_Move(SourceAsset)));

							nAssets += Assets.f_GetLen();

							co_yield fg_Move(Assets);
						}

						bDone = true;

						co_return {};
					}
				)
			}
		;

		Auditor.f_Info("Started listing of assets");

		co_return {};
	}
}
