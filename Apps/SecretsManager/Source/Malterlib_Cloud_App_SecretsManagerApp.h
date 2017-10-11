// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Concurrency/DistributedApp>

namespace NMib::NCloud
{
	NConcurrency::TCActor<NConcurrency::CDistributedAppActor> fg_ConstructApp_SecretsManager();
}
