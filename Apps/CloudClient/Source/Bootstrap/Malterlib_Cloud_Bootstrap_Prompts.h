// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/CommandLine/AnsiEncoding>
#include <Mib/Container/Vector>
#include <Mib/Storage/Optional>
#include <Mib/String/String>

namespace NMib::NCloud::NBootstrap
{
	TCFuture<CStr> fg_PromptWithDefault
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, CStr _CurrentValue
			, CStr _Prompt
			, CStr _Default
		)
	;

	// Single-select: Returns TCOptional<CStr> with the selected item (first column)
	// Returns nullopt on cancel (Escape), empty optional on empty list
	TCFuture<TCOptional<CStr>> fg_SelectFromListWithFilter
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, TCVector<TCVector<CStr>> _Items
			, TCVector<CStr> _Headings
			, CStr _Prompt
			, CStr _Default = {}
		)
	;

	// Multi-select: Returns TCOptional<TCVector<CStr>> with all selected items (first column values)
	// Space toggles selection, Ctrl+A selects all visible, Ctrl+D deselects all
	// Returns nullopt on cancel (Escape), empty vector is valid (user confirmed with no selections)
	TCFuture<TCOptional<TCVector<CStr>>> fg_MultiSelectFromListWithFilter
		(
			TCSharedPointer<CCommandLineControl> _pCommandLine
			, TCVector<TCVector<CStr>> _Items
			, TCVector<CStr> _Headings
			, CStr _Prompt
			, TCVector<CStr> _DefaultSelected = {}
		)
	;
}
