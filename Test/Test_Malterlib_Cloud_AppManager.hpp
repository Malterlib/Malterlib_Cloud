// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CHostID, typename tf_CPermissions>
	auto CAppManagerTestHelper::fs_Permissions(tf_CHostID &&_HostID, tf_CPermissions &&_Permissions)
	{
		return CDistributedActorTrustManagerInterface::CAddPermissions{{_HostID, ""}, _Permissions, mc_WaitForSubscriptions};
	}

	template <typename tf_CNamespace, typename tf_CHosts>
	auto CAppManagerTestHelper::fs_NamespaceHosts(tf_CNamespace &&_Namespace, tf_CHosts &&_Hosts)
	{
		return CDistributedActorTrustManagerInterface::CChangeNamespaceHosts{_Namespace, _Hosts, mc_WaitForSubscriptions};
	}
}
