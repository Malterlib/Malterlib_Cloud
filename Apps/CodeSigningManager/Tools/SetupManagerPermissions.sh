#!/bin/bash
# Copyright © Unbroken AB
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# setup_codesign_permissions.sh - Set up CodeSigningManager permissions in MalterlibCloud
#
# This script configures the necessary permissions for CodeSigningManager to manage
# certificate authorities and signing certificates through the SecretsManager.
#
# Usage: setup_codesign_permissions.sh [options]
#
# Options:
#   --host HOST_ID            Host ID for permissions (required)
#   --authority-user USER_ID  Authority management user ID (optional, default: AuthorityManagement)
#   --signingcert-user USER_ID Signing cert create user ID (optional, default: SigningCertCreate)
#   --authority-pattern PATTERN Authority pattern (default: * for all)
#   --signingcert-pattern PATTERN Signing cert pattern (default: * for all)
#   --auth-factors FACTORS    Authentication factors (default: Password)
#   --auth-lifetime MINUTES   Authentication lifetime in minutes (default: 10)
#   --secrets-manager PATH    Path to SecretsManager binary (default: /opt/Deploy/Malterlib_All/SecretsManager)
#   --dry-run                 Show commands without executing
#   --help                    Show this help message

set -e

# Default values
SECRETS_MANAGER="/opt/Deploy/Malterlib_All/SecretsManager"
AUTH_LIFETIME="10"
AUTHORITY_USER=""
SIGNINGCERT_USER=""
AUTHORITY_PATTERN="*"
SIGNINGCERT_PATTERN="*"
AUTH_FACTORS="Password"
AUTHORITY_AUTH_FACTORS=""  # Will use AUTH_FACTORS if not specified
SIGNINGCERT_AUTH_FACTORS=""  # Will use AUTH_FACTORS if not specified
DRY_RUN=0

AUTHORITY_PREFIX="org.malterlib.codesign.authority"
SIGNINGCERT_PREFIX="org.malterlib.codesign.signingcert"
REMOVE_PERMISSIONS=0

# Parse command line arguments
parse_args() {
	while [[ $# -gt 0 ]]; do
		case $1 in
			--host)
				HOST_ID="$2"
				shift 2
				;;
			--authority-user)
				AUTHORITY_USER="$2"
				shift 2
				;;
			--signingcert-user)
				SIGNINGCERT_USER="$2"
				shift 2
				;;
			--authority-pattern)
				AUTHORITY_PATTERN="$2"
				shift 2
				;;
			--signingcert-pattern)
				SIGNINGCERT_PATTERN="$2"
				shift 2
				;;
			--auth-factors)
				AUTH_FACTORS="$2"
				shift 2
				;;
			--authority-auth-factors)
				AUTHORITY_AUTH_FACTORS="$2"
				shift 2
				;;
			--signingcert-auth-factors)
				SIGNINGCERT_AUTH_FACTORS="$2"
				shift 2
				;;
			--auth-lifetime)
				AUTH_LIFETIME="$2"
				shift 2
				;;
			--secrets-manager)
				SECRETS_MANAGER="$2"
				shift 2
				;;
			--dry-run)
				DRY_RUN=1
				shift
				;;
			--remove)
				REMOVE_PERMISSIONS=1
				shift
				;;
			--help)
				show_help
				exit 0
				;;
			*)
				echo "Error: Unknown option $1" >&2
				echo "Use --help for usage information" >&2
				exit 1
				;;
		esac
	done
}

show_help() {
	cat << EOF
setup_codesign_permissions.sh - Set up CodeSigningManager permissions in MalterlibCloud

This script configures the necessary permissions for CodeSigningManager to manage
certificate authorities and signing certificates through the SecretsManager.

Usage: setup_codesign_permissions.sh [options]

Required Options:
  --host HOST_ID               Host ID for permissions (e.g., SPkHzRrY5Eo5SAnG5)

Optional Options:
  --authority-user USER_ID     Authority management user ID
                               Default: AuthorityManagement
  --signingcert-user USER_ID   Signing cert create user ID
                               Default: SigningCertCreate
  --authority-pattern PATTERN  Authority pattern (e.g., * for all, or specific names)
                               Default: *
  --signingcert-pattern PATTERN Signing cert pattern
                               Default: *
  --auth-factors FACTORS       Authentication factors for both users (comma-separated or JSON)
                               Default: Password
                               Available: Password, U2F
                               Examples:
                                 Password               - Single factor
                                 Password,U2F           - Either Password OR U2F
                                 [Password,U2F]         - Either Password OR U2F (JSON)
                                 [[Password,U2F]]       - Both Password AND U2F (JSON)
  --authority-auth-factors     Override auth factors for authority user only
                               Default: uses --auth-factors value
  --signingcert-auth-factors   Override auth factors for signing cert user only
                               Default: uses --auth-factors value
  --auth-lifetime MINUTES      Authentication lifetime in minutes
                               Default: 10
  --secrets-manager PATH       Path to SecretsManager binary
                               Default: /opt/Deploy/Malterlib_All/SecretsManager
  --dry-run                    Show commands without executing
  --help                       Show this help message

Examples:
  # Set up permissions for all authorities and signing certs (Password auth)
  ./setup_codesign_permissions.sh --host SPkHzRrY5Eo5SAnG5

  # Set up permissions for specific authority pattern
  ./setup_codesign_permissions.sh --host SPkHzRrY5Eo5SAnG5 \\
    --authority-pattern "prod-*" --signingcert-pattern "prod-*"

  # Use U2F hardware token authentication for all users
  ./setup_codesign_permissions.sh --host SPkHzRrY5Eo5SAnG5 \\
    --auth-factors "U2F"

  # Require both Password AND U2F for authority, Password only for signing certs
  ./setup_codesign_permissions.sh --host SPkHzRrY5Eo5SAnG5 \\
    --authority-auth-factors "[[Password,U2F]]" \\
    --signingcert-auth-factors "Password"

  # Different auth methods: U2F for authority, Password for signing certs
  ./setup_codesign_permissions.sh --host SPkHzRrY5Eo5SAnG5 \\
    --authority-auth-factors "U2F" --signingcert-auth-factors "Password"

  # Dry run to see what commands would be executed
  ./setup_codesign_permissions.sh --host SPkHzRrY5Eo5SAnG5 --dry-run

Permissions that will be configured:

1. Authority Management (--authority-user):
   - Read/Write: org.malterlib.codesign.authority#PATTERN/Tag/Private
   - Read/Write: org.malterlib.codesign.authority#PATTERN/Tag/Public
   - Command: SetSecretProperties
   Authentication: AUTH_FACTORS, Lifetime: AUTH_LIFETIME minutes

2. Signing Certificate Creation (--signingcert-user):
   - Read/Write: org.malterlib.codesign.signingcert#PATTERN/Tag/Private
   - Read/Write: org.malterlib.codesign.signingcert#PATTERN/Tag/Public
   - Read: org.malterlib.codesign.authority#PATTERN/Tag/Public
   - Read: org.malterlib.codesign.authority#PATTERN/Tag/Private
   Authentication: AUTH_FACTORS, Lifetime: AUTH_LIFETIME minutes

EOF
}

validate_args() {
	local errors=0

	if [[ -z "$HOST_ID" ]]; then
		echo "Error: --host HOST_ID is required" >&2
		errors=1
	fi

	if [[ -z "$AUTHORITY_USER" ]]; then
		echo "Error: --authority-user AUTHORITY_USER_ID is required" >&2
		errors=1
	fi

	if [[ -z "$SIGNINGCERT_USER" ]]; then
		echo "Error: --signingcert-user SIGNINGCERT_USER_ID is required" >&2
		errors=1
	fi

	if [[ ! -x "$SECRETS_MANAGER" ]]; then
		echo "Error: SecretsManager not found or not executable at: $SECRETS_MANAGER" >&2
		errors=1
	fi

	if [[ $errors -eq 1 ]]; then
		echo "" >&2
		echo "Use --help for usage information" >&2
		exit 1
	fi
}

format_auth_factors() {
	local factors="$1"

	# If it's already in JSON format (starts with [ or [[)
	if [[ "$factors" =~ ^\[.*\]$ ]]; then
		# Check if strings inside are already quoted
		if [[ "$factors" =~ \".*\" ]]; then
			# Already has quotes, use as-is
			echo "$factors"
		else
			# Need to add quotes to unquoted strings inside JSON arrays
			# This handles formats like [[Password,U2F]] or [Password,U2F]
			local result=""
			local in_word=false
			local current_word=""

			for (( i=0; i<${#factors}; i++ )); do
				local char="${factors:$i:1}"

				if [[ "$char" =~ [A-Za-z0-9_] ]]; then
					# Part of a word
					in_word=true
					current_word+="$char"
				else
					# Not part of a word
					if [ "$in_word" = true ]; then
						# End of word, add quotes
						result+="\"$current_word\""
						current_word=""
						in_word=false
					fi
					result+="$char"
				fi
			done

			# Handle any remaining word
			if [ "$in_word" = true ]; then
				result+="\"$current_word\""
			fi

			echo "$result"
		fi
	# If it contains comma, treat as OR condition (either factor)
	elif [[ "$factors" == *","* ]]; then
		# Convert comma-separated to JSON array format with proper quoting
		IFS=',' read -ra FACTOR_ARRAY <<< "$factors"
		local json_array="["
		local first=true
		for factor in "${FACTOR_ARRAY[@]}"; do
			# Trim whitespace
			factor=$(echo "$factor" | xargs)
			if [ "$first" = true ]; then
				json_array+="\"$factor\""
				first=false
			else
				json_array+=",\"$factor\""
			fi
		done
		json_array+="]"
		echo "$json_array"
	# Single factor - pass as plain string (no JSON quoting)
	else
		echo "$factors"
	fi
}

run_command() {
	local cmd="$1"

	if [[ $DRY_RUN -eq 1 ]]; then
		echo "[DRY-RUN] $cmd"
	else
		echo "Executing: $cmd"
		eval "$cmd"
	fi
}

setup_permissions() {
	# Use specific auth factors if provided, otherwise fall back to default
	local authority_factors="${AUTHORITY_AUTH_FACTORS:-$AUTH_FACTORS}"
	local signingcert_factors="${SIGNINGCERT_AUTH_FACTORS:-$AUTH_FACTORS}"

	# Format authentication factors for use in commands
	local formatted_authority_factors=$(format_auth_factors "$authority_factors")
	local formatted_signingcert_factors=$(format_auth_factors "$signingcert_factors")

	local authority_public_pattern="${AUTHORITY_PREFIX}#$AUTHORITY_PATTERN"
	local authority_private_pattern="${AUTHORITY_PREFIX}#$AUTHORITY_PATTERN"
	local signingcert_public_pattern="${SIGNINGCERT_PREFIX}#$SIGNINGCERT_PATTERN"
	local signingcert_private_pattern="${SIGNINGCERT_PREFIX}#$SIGNINGCERT_PATTERN"

	echo "Setting up CodeSigningManager permissions..."
	echo "Host: $HOST_ID"
	echo "Authority User: $AUTHORITY_USER"
	echo "SigningCert User: $SIGNINGCERT_USER"
	echo "Authority Public Pattern: $authority_public_pattern"
	echo "Authority Private Pattern: $authority_private_pattern"
	echo "SigningCert Public Pattern: $signingcert_public_pattern"
	echo "SigningCert Private Pattern: $signingcert_private_pattern"
	echo "Authority Auth Factors: $formatted_authority_factors"
	echo "SigningCert Auth Factors: $formatted_signingcert_factors"
	echo "Authentication Lifetime: $AUTH_LIFETIME minutes"
	echo ""

	# Common
	echo "=== Setting up Common permissions host ==="

	local permission_command="--trust-permission-add"

	if [[ $REMOVE_PERMISSIONS -eq 1 ]]; then
		permission_command="--trust-permission-remove"
	fi

	function onlyAdd() {
		if [[ $REMOVE_PERMISSIONS -eq 1 ]]; then
			return;
		fi

		echo "$@"
	}

	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		-- 'SecretsManager/Command/SubscribeToChanges'"

	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		-- 'SecretsManager/Command/GetSecretProperties'"

	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		-- 'SecretsManager/Command/GetSecret'"

	# Authority Management Permissions with authentication
	echo "=== Setting up Authority Management permissions for user $AUTHORITY_USER ==="

	# SetSecretProperties command with auth
	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		--user '$AUTHORITY_USER' \\
		`onlyAdd --authentication-factors '$formatted_authority_factors'` \\
		`onlyAdd --max-lifetime '$AUTH_LIFETIME'` \\
		-- 'SecretsManager/Command/SetSecretProperties'"

	# Read/Write authority private keys with auth
	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		--user '$AUTHORITY_USER' \\
		`onlyAdd --authentication-factors '$formatted_authority_factors'` \\
		`onlyAdd --max-lifetime '$AUTH_LIFETIME'` \\
		-- 'SecretsManager/Read/SemanticID/${authority_private_pattern}/Tag/Private'"

	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		--user '$AUTHORITY_USER' \\
		`onlyAdd --authentication-factors '$formatted_authority_factors'` \\
		`onlyAdd --max-lifetime '$AUTH_LIFETIME'` \\
		-- 'SecretsManager/Write/SemanticID/${authority_private_pattern}/Tag/Private'"

	# Read authority public secrets without auth
	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		-- 'SecretsManager/Read/SemanticID/${authority_public_pattern}/Tag/Public'"

	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		--user '$AUTHORITY_USER' \\
		`onlyAdd --authentication-factors '$formatted_authority_factors'` \\
		`onlyAdd --max-lifetime '$AUTH_LIFETIME'` \\
		-- 'SecretsManager/Write/SemanticID/${authority_public_pattern}/Tag/Public'"

	echo ""
	echo "=== Setting up Signing Certificate permissions for user $SIGNINGCERT_USER ==="

	# Read/Write signing cert private keys with auth
	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		--user '$SIGNINGCERT_USER' \\
		`onlyAdd --authentication-factors '$formatted_signingcert_factors'` \\
		`onlyAdd --max-lifetime '$AUTH_LIFETIME'` \\
		-- 'SecretsManager/Command/SetSecretProperties'"

	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		--user '$SIGNINGCERT_USER' \\
		`onlyAdd --authentication-factors '$formatted_signingcert_factors'` \\
		`onlyAdd --max-lifetime '$AUTH_LIFETIME'` \\
		-- 'SecretsManager/Read/SemanticID/${signingcert_private_pattern}/Tag/Private'"

	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		--user '$SIGNINGCERT_USER' \\
		`onlyAdd --authentication-factors '$formatted_signingcert_factors'` \\
		`onlyAdd --max-lifetime '$AUTH_LIFETIME'` \\
		-- 'SecretsManager/Write/SemanticID/${signingcert_private_pattern}/Tag/Private'"

	# Read authority private keys (for CSM operations, auth required to sign new signing cert)
	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		--user '$SIGNINGCERT_USER' \\
		`onlyAdd --authentication-factors '$formatted_signingcert_factors'` \\
		`onlyAdd --max-lifetime '$AUTH_LIFETIME'` \\
		-- 'SecretsManager/Read/SemanticID/${authority_private_pattern}/Tag/Private'"

	# Write signing cert (all tags) with auth
	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		--user '$SIGNINGCERT_USER' \\
		`onlyAdd --authentication-factors '$formatted_signingcert_factors'` \\
		`onlyAdd --max-lifetime '$AUTH_LIFETIME'` \\
		-- 'SecretsManager/Write/SemanticID/${signingcert_public_pattern}/Tag/Public'"

	# Read signing cert public secrets without auth
	run_command "$SECRETS_MANAGER $permission_command \\
		--host '$HOST_ID' \\
		-- 'SecretsManager/Read/SemanticID/${signingcert_public_pattern}/Tag/Public'"

	echo ""
	if [[ $DRY_RUN -eq 1 ]]; then
		echo "Dry run complete. No permissions were actually modified."
	else
		echo "Permissions setup complete!"
	fi

	echo ""
	echo "Note: You may also need to set up permissions for MalterlibCloud host if it's different."
	echo "The template in CodeSign.md shows additional permissions that may be needed for:"
	echo "  - SecretsManager/CommandAll for MalterlibCloud host"
	echo "  - SecretsManager/Read/* and SecretsManager/Write/* for MalterlibCloud host"
}

# Main execution
parse_args "$@"
validate_args
setup_permissions
