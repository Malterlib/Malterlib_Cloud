#!/bin/bash
# Copyright © Unbroken AB
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set -e

# Generate at least 24 MB of error
Errors="$(printf 'error %.0s' {1..90})"
Errors="${Errors% }"
for ((i = 0 ; i < 50000 ; i++ )); do
	echo "$i: $Errors"
done

exit 1
