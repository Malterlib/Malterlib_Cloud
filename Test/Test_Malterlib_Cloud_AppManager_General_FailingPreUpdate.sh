R"-----(#!/bin/bash

set -e

# Generate at least 24 MB of error
for ((i = 0 ; i < 50000 ; i++ )); do
	echo "$i: error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error error"
done

exit 1

)-----"
