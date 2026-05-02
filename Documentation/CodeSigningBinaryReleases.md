# Code Signing Binary Releases

## Goal

Enable Malterlib binary releases to be signed and verified without giving build runners access to private signing keys.

Signing uses the existing `CodeSigningManager` infrastructure. Build runners request signatures through `MalterlibCloud --code-signing-manager-sign-files`; signing private keys stay on signing `CodeSigningManager` hosts and in `SecretsManager` backed storage.

## Current Branch State

Implemented pieces:

- `NCryptography::fg_SignFiles` signs a file or directory and returns signature JSON.
- `NCryptography::fg_VerifyFiles` verifies a file or directory against signature JSON and trusted CA certificates.
- Signature JSON contains a manifest, certificate, digest algorithm, signature, message length, and optional timestamp token.
- Timestamp support uses RFC 3161 TSA responses.
- `CCodeSigningManager` exposes distributed signing.
- `MalterlibCloud --code-signing-manager-sign-files` uploads files to a trusted `CodeSigningManager` and writes signature JSON.
- `CodeSigningManager` can create and manage certificate authorities and signing certificates through `SecretsManager`.

Missing pieces:

- Build-system integration that signs produced files automatically.
- Bootstrap verification before installing downloaded binaries.
- Repository-configured signature verification.
- MTool build integration for a baked trusted CA bundle.
- Tests for the build, release, bootstrap, and repository signature-verification flow.

## Architecture

Build runners do not hold private keys.

Normal signing flow:

```text
BuildRunner
  -> build system post-build/staging script
  -> MalterlibCloud --code-signing-manager-sign-files
  -> signing CodeSigningManager
  -> SecretsManager
```

Build-server signing flow:

```text
BuildRunner
  -> MTool BuildServerTool Tool=MalterlibCloud
  -> MalterlibCloud --code-signing-manager-sign-files
  -> signing CodeSigningManager
  -> SecretsManager
```

This follows the existing `MalterlibCloud_CloudClientCommand` and `MTool BuildServerTool Tool=MalterlibCloud` pattern used for version uploads.

## Build-Time Signing

Files should be signed directly when the build system produces or stages them, before release-package upload.

For each trusted binary output:

- Build the file normally.
- Run `MalterlibCloud --code-signing-manager-sign-files`.
- Write an adjacent signature file named `<file>.signature.json`.
- Treat the original file and signature JSON as build outputs.

Example command shape:

```bash
MalterlibCloud --code-signing-manager-sign-files \
	--input "<file>" \
	--output "<file>.signature.json" \
	--authority "<authority>" \
	--signing-cert "<signing-cert>"
```

Build-server command shape:

```bash
MTool BuildServerTool Tool=MalterlibCloud \
	--code-signing-manager-sign-files \
	--input "<file>" \
	--output "<file>.signature.json" \
	--authority "<authority>" \
	--signing-cert "<signing-cert>"
```

No `--sign-executable` alias is needed. `--code-signing-manager-sign-files` is the command surface for this work.

## Build-System Properties

Add signing command properties parallel to the current Cloud version-upload command properties.

Proposed shape:

```malterlib-build
Property
{
	MalterlibCloud_CodeSigningToolName: string? = "MalterlibCloud"
	MalterlibCloud_CodeSigningCommand: string = "false"

	MalterlibCloud_CodeSigningCommand MalterlibCloud_CloudClientExecutable
	{
		!MalterlibCloud_CloudClientExecutable undefined
	}

	MalterlibCloud_CodeSigningCommand `@(MalterlibBuildServerCommandPrefix)MTool BuildServerTool Tool=@(MalterlibCloud_CodeSigningToolName)`
	{
		&
		{
			MalterlibBuildServerBuild true
			!MalterlibCloud_CodeSigningToolName undefined
		}
	}
}
```

Additional properties will be needed for authority, signing certificate, host selection, and per-target opt-in.

## Trusted CAs

Trusted CA roots used for repository verification should be built into `MTool`, not supplied by the repository being verified. The MTool build should declare a list of trusted CA certificate paths in the DSL and embed those certificate contents into the `MTool` executable as part of the build, using the existing embedded-file mechanism used by `Malterlib_Tool_MTool`. Runtime verification uses the embedded certificate contents, not the build-time file paths.

Bootstrap verification is the exception because `MTool` does not exist yet. The `Malterlib/Core/mib` script should manually contain the trusted bootstrap CA material needed to verify the downloaded bootstrap package. That manually embedded CA set is maintained in source alongside the script and is used only for bootstrap verification before the downloaded `MTool` is trusted.

Proposed MTool build property:

```malterlib-build
Property
{
	MToolTrustedSignatureCAs: [string] = []
}

%Target "Malterlib_Tool_MTool"
{
	Property
	{
		MToolTrustedSignatureCAs =+ ["Malterlib/Core/Certificates/MalterlibReleaseCA.crt"->MakeAbsolute()]
	}
}
```

At runtime, `MTool VerifyRepositorySignatures` should trust:

- CA certificates baked into the running `MTool` binary.
- User-added CA certificates from a documented location under `~/.Malterlib`.

Repository DSL, the signed repository manifest, and the Git-common-dir verification state must not define trusted CA paths. Trust roots come from the verifier binary and the user's local trust store.

The bootstrap `mib` script should not load user-added CAs for initial bootstrap verification. User-added CAs are a post-bootstrap `MTool` trust extension for repository verification.

## BuildServerTool Output Copyback

`MTool BuildServerTool` already uploads file and directory arguments, runs the named tool on the build server, and copies back uploaded arguments reported as changed by the remote tool.

Signing creates a new output file (`<file>.signature.json`), so the build-server path must ensure that output is copied back.

Preferred implementation:

- Extend `BuildServerTool` with explicit output-file support.
- Upload only required inputs.
- After the tool runs, download declared output files from the build server.

Minimal implementation if output-file support is deferred:

- Create an empty signature file before invoking `BuildServerTool`.
- Pass the signature file as a tracked argument.
- Let the remote `MalterlibCloud` overwrite it.
- Rely on changed-file copyback.

The preferred implementation is safer and less surprising for future tools that create outputs remotely.

## Release Package Layout

The release package should include signatures inside `MTool.tar.gz`. The `ReleasePackage.Packages.Files` list in `Malterlib_Core_RepositoryBinary.MHeader` should include repository files: each signed binary, its adjacent signature JSON, expected aliases such as `mib`, a bootstrap package-bound JSON file, and the package-bound JSON signature.

`MTool` build artifacts go into the `Binaries/Malterlib` repository first, and the release package is generated from that repository state. The release-package generation step must not require signing keys. All signed artifacts needed by bootstrap, including `MToolBootstrapPackage.bound.json` and `MToolBootstrapPackage.bound.json.signature.json`, must already be present in the repository before the package is generated.

`mib` is an alias/symlink to `MTool`, not a separately signed executable. Bootstrap should verify the signed `MTool` file and verify that the `mib` entry is the expected alias to `MTool`. The bootstrap package-bound JSON should record expected aliases separately from signed regular files.

The bootstrap package-bound JSON is separate from repository `.manifest.json`. Repository `.manifest.json` files stay in repositories only and describe repository signature policy. The MTool build/signing flow generates the bootstrap package-bound JSON from the repository files that will be packaged, including file digests and symlink metadata, signs that package-bound JSON, and applies both files to the repository. The later release-package step only archives those repository files.

Example package contents on macOS:

```text
Bootstrap.version
MToolBootstrapPackage.bound.json
MToolBootstrapPackage.bound.json.signature.json
MTool
MTool.signature.json
mib
bsdtar
bsdtar.signature.json
MalterlibHelper
MalterlibHelper.signature.json
MalterlibOverrideMalloc.dylib
MalterlibOverrideMalloc.dylib.signature.json
LICENSE
```

Example package contents on Linux:

```text
Bootstrap.version
MToolBootstrapPackage.bound.json
MToolBootstrapPackage.bound.json.signature.json
MTool
MTool.signature.json
mib
bsdtar
bsdtar.signature.json
MalterlibHelper
MalterlibHelper.signature.json
LICENSE
```

Example package contents on Windows:

```text
Bootstrap.version
MToolBootstrapPackage.bound.json
MToolBootstrapPackage.bound.json.signature.json
MTool.exe
MTool.exe.signature.json
mib.exe
bsdtar.exe
bsdtar.exe.signature.json
LICENSE
```

The tarball itself does not need an external signature for this design. Bootstrap verifies a signed package-bound JSON and then verifies the unpacked trusted files before installing them.

The signed package-bound JSON must bind the archive contents to the requested bootstrap identity and to the exact bytes being installed. It should include at least the bootstrap version, platform, architecture, required signed regular-file list, per-signed-file digests, expected payload signature-file list, per-payload-signature-file digests, allowed unsigned regular-file list with per-file digests, expected alias/symlink list, and whether timestamps are required. Unsigned regular files should be limited to control or documentation files such as `Bootstrap.version` and `LICENSE`. Bootstrap must reject an archive whose signed package-bound JSON does not match the requested `bootstrap/<version>` asset, host platform, host architecture, expected signed payload bytes, expected payload signature bytes, expected unsigned-file bytes, and expected aliases. This prevents replaying older legitimately signed binaries or unsigned control files in a newer release asset.

The package-bound JSON digests are separate from the adjacent per-file signatures. The package-bound JSON says which exact signed binaries, payload signature files, and allowed unsigned regular files belong to this bootstrap release; the adjacent signatures prove publisher authenticity for the signed payload files. The package-bound JSON must not include a digest of its own signature file (`MToolBootstrapPackage.bound.json.signature.json`), because that would create a circular dependency. The package-bound JSON signature is verified directly against the package-bound JSON bytes and trusted bootstrap CA material.

## Bootstrap Verification

Bootstrap must not extract directly into the final bootstrap directory.

Required flow:

```text
download MTool.tar.gz
pre-scan archive entries and reject unsafe entry types or paths
extract only the package-bound JSON and package-bound JSON signature into a fresh temporary directory
verify signed package-bound JSON using the CA material manually embedded in Malterlib/Core/mib
verify package-bound JSON matches requested bootstrap version, platform, architecture, required signed file set, signature-file set, allowed unsigned file set, and alias set
extract remaining allowed archive entries into the fresh temporary directory
verify extracted signed-file, unsigned-file, and signature-file digests against the package-bound JSON
verify expected unpacked files against adjacent signature JSON using the CA material manually embedded in Malterlib/Core/mib
verify expected aliases point to the signed files named in the package-bound JSON
fail if verification fails
fail if an expected signature is missing
fail if unexpected executable or binary files are present
copy verified contents into ~/.Malterlib/bootstrap/<version>
run installed tool only after verification
```

Extraction security requirements:

- Extract only to a fresh temporary directory.
- Before verifying the package-bound JSON, extract only `MToolBootstrapPackage.bound.json` and `MToolBootstrapPackage.bound.json.signature.json` as regular files.
- Before extracting remaining entries, inspect the tar table of contents and reject anything except regular files and aliases/symlinks explicitly listed in the already verified package-bound JSON, plus directories required to contain those entries.
- Reject or safely handle absolute paths.
- Reject or safely handle `..` path traversal.
- Reject unsafe symlinks. Only the exact expected alias entries from the signed package-bound JSON are allowed, such as `mib -> MTool` or `mib.exe -> MTool.exe`.
- Reject hardlinks.
- Reject device nodes, FIFOs, sockets, and other special entries.
- Do not execute anything from the extracted archive before verification succeeds.

First-bootstrap trust is rooted in the CA material manually baked into `Malterlib/Core/mib`. A downloaded `MTool` must never verify itself before it is trusted.

The manually embedded bootstrap CA material should be intentionally small and updated only through source changes to the script. If the bootstrap CA set changes, the script update must land before packages requiring the new CA are published.

Assuming Git Bash on Windows includes `openssl`, bootstrap verification can be implemented directly in the `mib` shell script with platform-default tools plus OpenSSL. The script should fail early with a clear error if `openssl` is missing or does not support the required commands. This makes the bootstrap prerequisite explicit instead of silently falling back to unsigned bootstrap.

Bootstrap shell verification should keep the parsed format deliberately simple:

- Use the CA material embedded in `Malterlib/Core/mib` as an OpenSSL CA file.
- Extract signature JSON fields with narrow, validated parsing or provide a bootstrap-specific sidecar format that is easier to parse safely in shell.
- Decode certificate, signature, and timestamp token fields with OpenSSL or POSIX tools available in Git Bash/macOS/Linux.
- Verify the signing certificate chain with OpenSSL against the embedded CA material.
- Verify the detached file signature with OpenSSL.
- Verify the timestamp token with OpenSSL if timestamp validation is required for bootstrap.

If matching the full `NCryptography::fg_VerifyFiles` JSON and directory-manifest semantics in shell becomes too brittle, bootstrap should use a constrained bootstrap-specific signature sidecar while repository verification continues to use the full `MTool` implementation.

After bootstrap has synced the `Binaries/Malterlib` repository, it should use the already bootstrapped `MTool` to verify that repository before any binaries from the repository are used.

## Repository Signature Verification

Files downloaded from repositories with signature verification enabled must be verified after Git and LFS materialize them.

This is separate from LFS object hashing. LFS verifies content integrity for an object ID, while code signing verifies publisher authenticity and timestamped validity.

Proposed repository signature policy generation should be configured as a repository DSL object, following the same style as `Repository.License`.

Proposed repository type and property shape:

```malterlib-build
Type
{
	CSignatureVerificationAlias: {
		Path: string
		, Target: string
	}

	CSignatureVerificationConfig: {
		Include: [string] = ["*"]
		, Exclude: [string] = ["*.signature.json"]
		, Aliases: [type(CSignatureVerificationAlias)] = []
		, AllowUnsignedInitialManifest: bool = false
		, RequireTimestamp: bool = true
		, ProposedManifestPath: string = ".manifest.proposed.json"
		, ManifestPath: string = ".manifest.json"
		, SignatureSuffix: string = ".signature.json"
	}
}

Repository
{
	SignatureVerification: type(CSignatureVerificationConfig)?
}
```

`Include` and `Exclude` are wildcard lists used to generate the proposed repository signature policy. After the proposed policy is actualized and signed, the committed signed `ManifestPath` is the authoritative policy. Signature files themselves should be excluded so the verifier does not require signatures for signatures. `ProposedManifestPath`, `ManifestPath`, `ManifestPath + SignatureSuffix`, and alias paths are always excluded from regular-file signature verification even if an `Include` wildcard would match them. The verifier locates `ManifestPath` and `ManifestPath + SignatureSuffix` from trusted local state or evaluated repository config before the manifest is trusted; only the wildcard and alias policy comes from the signed manifest during verification.

`Aliases` lists expected repository aliases/symlinks for proposed policy generation. Once actualized, the signed manifest's alias list is the authoritative policy and aliases are verified as metadata rather than as separately signed regular files. For phase one this covers `mib` pointing to `MTool` and, where applicable, `mib.exe` pointing to `MTool.exe`.

The repository-root manifest is a signed verification policy, not an expanded file-list or digest manifest. It should contain the wildcard policy directly, including at least repository identity, `Include`, `Exclude`, `Aliases`, `RequireTimestamp`, and `SignatureSuffix`. This avoids a circular dependency because the manifest contains only policy wildcards/settings, not hashes of files that are being signed.

Manifest flow:

- `update-repos` writes `ProposedManifestPath` from the evaluated repository DSL proposal.
- The proposed manifest is the handoff to the build and is not trusted for verification.
- Applying the build artifact to the repository copies `ProposedManifestPath` to `ManifestPath`, signs `ManifestPath`, and actualizes both `ManifestPath` and `ManifestPath + SignatureSuffix` in the repository.
- Verification uses only the actual signed `ManifestPath`, never the proposed manifest.

The manifest is not trusted by itself; `ManifestPath + SignatureSuffix` must exist and must verify against CAs trusted by the running `MTool`. After the manifest signature is trusted, verification must compare the signed manifest's repository identity with the Git-common-dir state so a manifest signed for a different repository cannot be replayed. It must then expand the signed wildcard policy against the checked-out repository, verify each in-scope regular file with its adjacent signature file, and verify configured aliases as metadata.

Hook/post-checkout verification must allow older signed repository policies so users can checkout old branches with older file lists. It should not reject a manifest solely because it differs from the latest DSL-generated proposed policy seen on another branch. Policy is whatever signed `ManifestPath` is committed in the checked-out repository state.

Verification state must be stored outside the worktree so a checkout cannot bypass verification by deleting `ManifestPath`. When a repository has `SignatureVerification` configured, `update-repos` writes a state file under the repository's Git common directory, for example `<git-common-dir>/malterlib/signature-verification-state.json`, before mutating the checkout. The only exception is temporary first-signing migration mode: if `AllowUnsignedInitialManifest` is true, no common-dir verification-required state exists yet, and no active committed signed `ManifestPath` exists yet, `update-repos` may write a non-mandatory migration-watch state instead of mandatory state so it can generate the initial proposed manifest. The state file is local metadata, not committed repository content.

Proposed-policy generation and enforcement are separate. In normal operation, enabling `SignatureVerification` means the repository must fail closed if `ManifestPath` or `ManifestPath + SignatureSuffix` is missing. The only exception is explicit first-signing migration mode, `AllowUnsignedInitialManifest`, which may be enabled temporarily to generate `ProposedManifestPath` before the first signed manifest is actualized. That flag must not suppress verification when common-dir verification state already exists or when an actual signed manifest is present, must not be used for steady-state verification, and should be removed once the signed manifest is committed.

The common-dir state should include enough local metadata to require or watch for verification without trusting the worktree, including repository identity, `ManifestPath`, `SignatureSuffix`, the trusted verifier path, and whether the state is mandatory or migration-watch only. It must not contain the include/exclude/alias verification policy. The signed manifest remains the source of the wildcard and alias policy after it has been verified and matched against the common-dir repository identity. The common-dir state is only the source of whether verification is mandatory and where to find the committed signed manifest.

If the common-dir state says verification is required and `ManifestPath` or `ManifestPath + SignatureSuffix` is missing from the worktree, verification must fail as a manifest verification failure. This prevents a malicious or stale checkout from making verification disappear by removing a previously active manifest.

Repositories with `SignatureVerification` configured should verify from the managed hook dispatcher itself so ordinary non-Malterlib Git operations still trigger verification. The dispatcher is `Malterlib_BuildSystem_Repository_HookDispatcher.sh`, embedded into `MTool` and installed as the managed wrapper in the Git common hooks directory. It should read the Git-common-dir state and call a trusted `MTool VerifyRepositorySignatures --root "<repository-root>"` when that state says verification is required. If the state is migration-watch only, the dispatcher should check whether `ManifestPath` and `ManifestPath + SignatureSuffix` have appeared after the Git operation; once they are present, it must invoke verification through the trusted verifier and promote the state to mandatory only after verification succeeds.

The dispatcher must not resolve `MTool` from `PATH` or from the repository being verified. For `Binaries/Malterlib`, that could execute the just-checked-out untrusted binary and let it report success. The common-dir state should include a trusted verifier path outside the repository, such as the current bootstrap or installed `MTool` path, and hooks must invoke that pinned path.

Required hooks:

- `post-checkout` for branch switches and file checkouts.
- `post-merge` for `git pull` and merges.
- `post-rewrite` for rebase and amend operations.

Post-operation hooks cannot reliably prevent the Git operation itself, so they are a hardening layer rather than the primary enforcement mechanism. On failure they must run the same executable-permission hardening as `update-repos` verification. Commit-time verification is not required; the important protection point is when a user moves the worktree to a new Git state through operations such as checkout, pull, merge, or rebase.

No separate verification hook scripts are needed. Existing hook management should still ensure the required hook types have the managed dispatcher installed, but repository signature verification should be built into the dispatcher rather than represented as additional scripts under `Repository.Hooks`. Existing hook ownership semantics should be preserved: hook types managed by Malterlib replace non-Malterlib repo-local hooks at those paths with a warning rather than chaining.

Phase one should enable this only for `Binaries/Malterlib`, by setting the object in the `%Repository MalterlibBinaryRepositories->Unique()` block in `Malterlib_Core_RepositoryBinary.MHeader`, adjacent to the existing `LfsReleaseStore` and `ReleasePackage` configuration.

Example phase-one configuration:

```malterlib-build
Repository
{
	SignatureVerification {
		Include: [
			"*"
		]
		, Exclude: [
			"*.signature.json"
			, "Bootstrap.version"
			, "LICENSE"
			, ".gitattributes"
		]
		, Aliases: RepoPlatformFamily->SwitchWithError(
			"Unsupported platform: "
			, "macOS", [{Path: "mib", Target: "MTool"}]
			, "Linux", [{Path: "mib", Target: "MTool"}]
			, "Windows", [{Path: "mib.exe", Target: "MTool.exe"}]
		)
		, AllowUnsignedInitialManifest: true // Temporary phase-one migration only; remove after first signed manifest is committed.
		, RequireTimestamp: true
		, ProposedManifestPath: ".manifest.proposed.json"
		, ManifestPath: ".manifest.json"
		, SignatureSuffix: ".signature.json"
	}
}
```

Out of scope for phase one:

- `Binaries/MalterlibLLVM`
- `Binaries/MalterlibSDK`
- `Binaries/MalterlibTest`

Verification timing:

- Write or refresh the Git-common-dir mandatory verification state before mutating the repository checkout when `SignatureVerification` is configured, unless `AllowUnsignedInitialManifest` is temporarily enabled, no mandatory state exists yet, and no active committed signed `ManifestPath` exists yet; in that migration case, write or refresh non-mandatory migration-watch state instead.
- Ensure the required hook types install the managed dispatcher, whose built-in verification path handles non-Malterlib Git operations.
- Generate or refresh the proposed repository-root policy manifest from DSL during `update-repos`; this does not change active verification policy until the proposal is actualized, signed, and committed.
- Actualize and sign the repository policy manifest when applying the build artifact to the repository.
- Apply the build artifact to the repository so the actual manifest and signature are present in the checkout.
- Run verification after repository checkout/update has completed.
- Run verification after LFS release-store downloads have materialized files.
- Fail repository update if verification fails.

Verification policy:

- Treat verification as mandatory when the Git-common-dir state says verification is required, when `SignatureVerification` is configured without `AllowUnsignedInitialManifest`, or when an actual signed `ManifestPath` is present in the checkout.
- Invoke verification through a trusted `MTool` path outside the repository being verified.
- Require the repository-root policy manifest and manifest signature file.
- Verify the policy manifest with `NCryptography::fg_VerifyFiles`.
- Reject the policy manifest if its repository identity does not match the Git-common-dir state.
- Expand the verified manifest's wildcard policy against the repository root.
- Require adjacent signature files for every in-scope regular file.
- Verify every in-scope file with `NCryptography::fg_VerifyFiles`.
- Verify configured aliases point to their expected targets and are not replaced by regular files or unsafe links.
- Trust only CAs baked into the running `MTool` binary plus user-added CAs under `~/.Malterlib`.
- Require timestamp by default.
- Reject modified content, missing manifest, missing manifest signature, wrong CA, missing timestamp, and malformed signature JSON.
- Quarantine unverified files before returning failure.
- If the manifest or manifest signature cannot be verified, quarantine all files in the repository before returning failure.

Quarantine must be effective on all supported platforms. On POSIX systems, removing executable permissions is acceptable for non-Windows binaries but may still be combined with renaming. On Windows, removing POSIX executable bits under Git Bash is not sufficient for `.exe` or `.dll` files, so verification failure should delete, rename, move to a quarantine directory, or apply Windows-effective ACL changes.

`MTool` should expose a separate command for verification so bootstrap can verify a synced repository using the already bootstrapped binaries:

```bash
MTool VerifyRepositorySignatures --root "<repository-root>"
```

The command should load the repository's Git-common-dir verification state, verify the signed policy manifest at the supplied root path when verification is mandatory or when migration-watch state sees the manifest present, expand the wildcard policy from the verified manifest, verify all matched files, apply the executable-permission hardening on failure, and return a non-zero exit code on failure. Trusted CAs must come from the running `MTool` binary and the user's local `~/.Malterlib` trust store, not from the repository config, signed manifest, or common-dir state.

When invoked with only `--root`, the command should read the Git-common-dir verification state for that repository. If the state requires verification, the command must verify even if the manifest is missing. If the state is migration-watch only, the command should verify and promote the state when the manifest and manifest signature are present, and should skip only while both are absent. If the state is missing, `update-repos` should recreate it before invoking verification when `SignatureVerification` is configured, except for temporary first-signing migration mode where `AllowUnsignedInitialManifest` is true and no active committed signed manifest exists yet; that exception should recreate migration-watch state instead.

`mib` should expose a repository-management command for manual and CI verification before pushing repository updates:

```bash
./mib repo-verify-signatures
```

The `repo-verify-signatures` command should select repositories with `SignatureVerification` configured, committed signed manifests, or local verification state, refresh the Git-common-dir verification state when needed, and invoke the same file verification policy as `update-repos`. It may skip or report repositories still in explicit `AllowUnsignedInitialManifest` migration mode when they have no committed signed manifest or local mandatory verification state. It is intended for humans and CI pipelines to validate the repository state they are about to push after applying build artifacts. It should support the standard repository filters used by other repository-management commands, such as repository name/type/tags where practical.

`repo-verify-signatures` may optionally report when the DSL-generated `ProposedManifestPath` differs from the committed signed `ManifestPath`, but that drift is not a verification failure by default. It means a proposed policy change has not yet been actualized by the signing build. The committed signed manifest remains the authoritative policy.

## Repository DSL Implementation Notes

The BuildSystem repository parser already reads repository configuration such as `LfsReleaseStore` and `ReleasePackage` into `CRemoteProperties` and repository metadata.

Repository signature verification should follow the same pattern:

- Add a typed `CSignatureVerificationConfig` DSL object with wildcard policy fields and the explicit `AllowUnsignedInitialManifest` migration guard.
- Store the evaluated `SignatureVerification` object only as input for proposed manifest generation.
- Write mandatory or migration-watch verification state under the repository Git common directory for signature-managed repositories.
- Add repository signature verification to the managed hook dispatcher embedded into `MTool`.
- Ensure signature-verification repositories install the managed dispatcher for the required hook types.
- Generate `SignatureVerification.ProposedManifestPath` from the evaluated DSL proposal during `update-repos` for configured repositories.
- Copy the proposed manifest to `SignatureVerification.ManifestPath` and sign it when applying the build artifact to the repository.
- Add an update-repos verification phase after checkout/LFS materialization.
- Add `MTool VerifyRepositorySignatures --root <repository-root>` for bootstrap, hook, and low-level verification.
- Add `./mib repo-verify-signatures` for manual and CI repository verification before push.
- Keep verification opt-in per repository.

Wildcard matching should mirror license checking behavior: match both repository-relative paths and bare file names so simple patterns like `MTool*` work regardless of directory layout.

The verification phase should report exact failing files and the configured repository that required verification. If the manifest is invalid, the report should clearly distinguish manifest verification failure from individual file verification failure.

## Cryptographic Verification Policy

Release and repository signature verification should enforce:

- Signature over file contents and manifest.
- Certificate chain to a CA trusted by the running `MTool`, either from the baked CA bundle or the user-local `~/.Malterlib` trust store.
- Code-signing EKU/key usage on signing certificates.
- Timestamp presence.
- Signing certificate validity at timestamp generation time.
- No unexpected files for directory verification.

The current `fg_VerifyFiles` already handles content, manifest, certificate-chain, signature, and timestamp validation. It may need hardening for release use around code-signing EKU/key usage and certificate validity at timestamp generation time.

## Implementation Plan

1. Add this design document.
2. Add Cloud code-signing command properties to the build DSL.
3. Add per-target or per-output signing opt-in for build outputs.
4. Ensure build-server signing copies generated signature JSON files back.
5. Add signed binaries, adjacent signatures, and signed package-bound JSON to the `Binaries/Malterlib` repository artifacts that feed `MTool.tar.gz`.
6. Add bootstrap archive pre-scan that rejects unsafe entry types and paths before extraction.
7. Add manually embedded bootstrap CA material to `Malterlib/Core/mib`.
8. Add bootstrap verification of unpacked package contents before install using the CA material embedded in `Malterlib/Core/mib`.
9. Add the typed `Repository.SignatureVerification` DSL config for proposed repository signature policy generation.
10. Enable proposed repository signature policy generation only for `Binaries/Malterlib` in phase one.
11. Add Git-common-dir verification state generation for signature-managed repositories during `update-repos`.
12. Add trusted verifier path storage in the Git-common-dir state.
13. Add repository signature verification to the managed hook dispatcher embedded into `MTool`.
14. Add repository-root proposed policy manifest generation from the repository DSL during `update-repos`.
15. Add repository manifest actualization, repository signing, and package-bound JSON signing to the build-artifact application flow.
16. Add repository update verification after checkout/LFS materialization.
17. Add `MTool VerifyRepositorySignatures --root <repository-root>`.
18. Add `./mib repo-verify-signatures` for manual and CI verification before push.
19. Use `MTool VerifyRepositorySignatures --root <repository-root>` from bootstrap after syncing the repository.
20. Add platform-effective quarantine hardening for verification failures.
21. Add MTool baked trusted CA bundle configuration and user-local CA loading from `~/.Malterlib`.
22. Harden verification policy for code-signing EKU/key usage and timestamp-time certificate validity.
23. Add tests for build signing, bootstrap verification, and repository signature verification.

## Tests

Required test coverage:

- Build output signing produces `<file>.signature.json`.
- Build-server signing copies generated signature JSON files back.
- Applying MTool build artifacts to `Binaries/Malterlib` writes `MToolBootstrapPackage.bound.json` and its signature into the repository.
- Release-package generation can produce `MTool.tar.gz` from repository files without access to signing keys.
- `MTool.tar.gz` contains expected signatures.
- `MTool.tar.gz` contains a signed package-bound JSON binding version, platform, architecture, required signed files, signed-file digests, payload signature files, payload signature-file digests, allowed unsigned files, unsigned-file digests, and expected aliases.
- Bootstrap verifies the package-bound JSON signature before using the package-bound JSON's alias allow-list to extract remaining archive entries.
- Bootstrap rejects replayed older signed binaries, unsigned files, or signature files whose digests do not match the signed package-bound JSON.
- Bootstrap rejects regular files not listed in the signed package-bound JSON as signed payloads, payload signatures, or allowed unsigned files.
- Bootstrap rejects package-bound JSON for the wrong bootstrap version, platform, or architecture.
- Bootstrap accepts only explicitly expected alias symlinks and rejects hardlinks, unexpected symlinks, device nodes, FIFOs, sockets, or other non-regular entries.
- Bootstrap verification uses CA material manually embedded in `Malterlib/Core/mib`.
- Bootstrap verification does not trust user-added CAs under `~/.Malterlib` for initial bootstrap.
- Bootstrap rejects modified unpacked files.
- Bootstrap rejects missing signatures.
- Bootstrap rejects wrong CA.
- Bootstrap rejects missing timestamp.
- Bootstrap rejects unsafe archive contents.
- Repository signature verification succeeds for valid `Binaries/Malterlib` contents.
- `update-repos` writes mandatory or migration-watch verification state under the repository Git common directory without storing include/exclude/alias policy there for configured repositories.
- Configured repositories fail closed on missing manifest files unless they are in explicit `AllowUnsignedInitialManifest` first-signing migration mode and no signed manifest or local mandatory verification state exists yet.
- `update-repos` ensures the managed hook dispatcher is installed for required hook types in configured repositories.
- The managed hook dispatcher invokes repository signature verification for required hook types when Git-common-dir state requires it.
- The managed hook dispatcher invokes a trusted `MTool` path outside the repository being verified.
- `post-checkout`, `post-merge`, and `post-rewrite` hooks invoke repository signature verification after non-Malterlib Git operations.
- Commit-time verification is intentionally not required; verification runs when Git operations move the worktree to a new state.
- Repository signature verification is still required when `.manifest.json` is removed from the worktree but common-dir state says verification is required.
- Repository signature verification requires a signed repository-root manifest.
- Repository signature verification rejects signed policy manifests whose repository identity does not match the current Git-common-dir state.
- Hook/post-checkout repository signature verification accepts older signed policy manifests so old branches with old file lists can still be checked out.
- Repository signature verification rejects missing manifest files.
- Repository signature verification rejects invalid manifest signatures.
- `update-repos` writes `.manifest.proposed.json` from the DSL without expanding repository file inventory or file digests; active policy does not change until the proposed manifest is signed and committed as `.manifest.json`.
- Applying the build artifact copies `.manifest.proposed.json` to `.manifest.json` and signs `.manifest.json`.
- Applying the build artifact actualizes `.manifest.json` and `.manifest.json.signature.json` in the repository.
- Repository signature verification uses the signed wildcard policy from the checked-out manifest.
- Repository signature verification rejects tampered content.
- Repository signature verification rejects missing manifest signature files.
- Repository signature verification rejects manifest signatures from untrusted CAs.
- Repository signature verification succeeds for signatures chained to a CA baked into `MTool`.
- Repository signature verification succeeds for signatures chained to a user-added CA under `~/.Malterlib`.
- Repository signature verification does not trust CA paths from repository DSL, the signed manifest, or Git-common-dir state.
- Repository signature verification quarantines failed files when file verification fails.
- Repository signature verification quarantines all repository files when manifest verification fails.
- Repository signature verification failure hardening is effective for Windows `.exe` and `.dll` files.
- `MTool VerifyRepositorySignatures --root <repository-root>` reads Git-common-dir verification state when invoked with only a root path.
- `MTool VerifyRepositorySignatures --root <repository-root>` succeeds and fails with the same policy as update-repos verification.
- `./mib repo-verify-signatures` verifies all selected repositories with configured `SignatureVerification`, committed signed manifests, or local verification state, except explicit unsigned-initial-manifest migration repositories that have no signed manifest or mandatory state yet.
- `./mib repo-verify-signatures` may report proposed/actual manifest drift but does not fail on that drift by default.
- `./mib repo-verify-signatures` is suitable for CI before pushing repository updates.

## Open Questions

- Which CA files should the initial `MToolTrustedSignatureCAs` DSL list bake into `MTool`?
- What exact user-local CA directory under `~/.Malterlib` should `MTool` load?
- How should the manually embedded bootstrap CA material in `Malterlib/Core/mib` be represented and updated?
- Should `BuildServerTool` output-file support be added before signing integration, or should the placeholder-output workaround be used temporarily?
- Which binaries in `Binaries/Malterlib` are mandatory signed files for each platform?
