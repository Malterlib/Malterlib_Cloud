# Plan: Launching AppManager Applications in Containers and VMs

Status: In implementation (2026-07-07)

Implementation progress:

- Phase 0 (done): `LaunchEnvironment` application setting, environment entities with
  settings/persistence/CLI/remote API/permissions, protocol version 0x11a.
- Phase 1 (done): environment agent mode (`MalterlibAppManagerEnvironmentAgentRoot`),
  `CAppManagerEnvironmentInterface`/`CAppManagerEnvironmentHostInterface` with reversed
  registration (the agent registers its interface through the host-published interface;
  WithID interfaces pair with WithID subscriptions), ephemeral applications in the agent,
  launch/stop/update-script delegation, agent-pushed status changes, Local environments
  with end-to-end tests, teardown and stale-agent cleanup. A trust manager init race with
  removed-while-connecting client connections was fixed in the Concurrency module.
- Phase 2 (machinery done): docker dialect container launches through the agent flow with
  pure, unit-tested argument building, root-directory bind mount, --env passthrough,
  host-gateway reachability, stale container removal. End-to-end container tests still
  require a cross-built Linux agent executable and docker and are not yet automated.
- Phases 3-5 (not started): Apple `container` dialect, `Malterlib/Virtualization` module
  with VM environments, and polish (statistics, coordinated agent updates, UI).

## 1. Goal and Scope

Allow the AppManager (`Malterlib/Cloud/Apps/AppManager`) to launch managed applications in
isolated **environments** instead of directly on the host:

| Host OS | Environment type | Technology |
|---------|------------------|------------|
| macOS | Linux container | Docker (Docker Desktop/colima) or Apple `container` CLI (Containerization framework, one lightweight VM per container) |
| macOS | macOS guest VM | Virtualization.framework (`VZVirtualMachine`) |
| Linux | Linux container | Docker |

Docker is supported on macOS as well as Linux — the docker dialect is one code path across
both hosts, so the container backend can be developed and tested on a macOS dev machine
before validating on Linux hosts.

Core design decisions (agreed):

1. Environments are **first-class managed entities**, created and controlled through their
   own command-line/remote API. Several applications can share one VM or container.
2. The **agent inside an environment is the AppManager itself**, installed as a daemon in
   the VM / run as the container's main process. It already has the machinery for account
   setup with correct permissions (macOS et al.), process launching, monitoring, limits and
   logging — refactored for reuse rather than duplicated.
3. Host ↔ agent communication uses **distributed actors** with its own automatic
   ticket-based connection, extending the existing AppManager-to-AppManager surface
   (`CAppManagerCoordinationInterface` pattern,
   `AppManager/Source/Malterlib_Cloud_App_AppManager_CoordinationInterface.h:11`,
   connected via `mp_State.m_TrustManager->f_SubscribeTrustedActors<...>()`,
   `Malterlib_Cloud_App_AppManager_CoordinatedState.cpp:13`).
4. Agent executables for guest platforms are provided by extending
   `--application-enable-self-update` (`Malterlib_Cloud_App_AppManager_CommandLine.cpp:463`)
   with options to install the AppManager distribution for **other platforms** as
   version-managed applications, so agent binaries stay current through the normal update
   pipeline.
5. No `CVirtualProcessLaunch`-factory seam in the Process module: its interface is
   synchronous and fits actors poorly. Container/VM control is handled explicitly in the
   AppManager with actors (`CProcessLaunchActor` for CLIs, a new Virtualization actor
   module for VZ).

Out of scope: Windows, orchestration (Kubernetes), building container images, running the
*root* AppManager inside a container.

## 2. Current Architecture (facts the design builds on)

- Launch entry point: `CAppManagerActor::fp_LaunchAppInternal`
  (`Malterlib_Cloud_App_AppManager_Launch.cpp:68`); external-process branch at
  `Launch.cpp:326` builds `CProcessLaunchParams` (cwd `<AppDir>`, `HOME=<AppDir>/.home`,
  `TMPDIR=<AppDir>/.tmp`, `m_RunAsUser/Group`) and launches through
  `CDistributedAppInterfaceLaunchActor : NProcess::CProcessLaunchActor`
  (`Concurrency/Source/DistributedApp/Malterlib_Concurrency_DistributedAppInterfaceLaunch.h:13`).
- The running launch is held in
  `TCVariant<void, TCActor<CDistributedAppInterfaceLaunchActor>, TCActor<CDistributedAppInProcessActor>> CApplication::m_ProcessLaunch`
  (`Malterlib_Cloud_App_AppManager.h:258`).
- Ticket handshake (`Malterlib_Concurrency_DistributedAppInterfaceLaunch.cpp:43-90`): child
  gets env vars `MalterlibDistributedAppInterfaceServerAddress` (`wss://[<hostname>]/`),
  `...RequestTicket`, `...LaunchID`; it prints a magic-prefixed ticket request on stdout and
  receives the ticket on stdin; the `FOnUseTicket` functor decides ticket grants.
- Crash/relaunch: `EProcessLaunchState_Launched/LaunchFailed/Exited` callbacks
  (`Launch.cpp:334-527`) drive `fp_ScheduleRelaunchApp` (`Launch.cpp:50`); stop goes through
  `CApplication::f_Stop` → `CProcessLaunchActor::f_StopProcess`
  (`Malterlib_Cloud_App_AppManager_Application.cpp:319`).
- Settings pattern: `EApplicationSetting` bit (`AppManager.h:57-96`), field in
  `CApplicationSettings` (`AppManager.h:107-152`), plumbing in
  `Malterlib_Cloud_App_AppManager_Settings.cpp` (`f_ParseSettings`/`f_ApplySettings`/
  `f_ChangedSettings`/`f_FromInterfaceAdd`/`f_FromInterfaceSettings`/`f_FromVersionInfo`/
  `f_Validate`), JSON persistence in `fp_UpdateApplicationJson`
  (`Malterlib_Cloud_App_AppManager_ApplicationManagement.cpp:190`), wire structs +
  protocol gate in `Cloud/Source/Malterlib_Cloud_AppManager.h:115-217`, CLI in
  `CommandLine.cpp`.
- App/user provisioning code that the agent will reuse: `fsp_CreateApplicationUserGroup`
  (`ApplicationManagement.cpp:336`), ownership/permission fixes
  (`ApplicationManagement.cpp:177-185`), limits (`Limits.cpp`), log/sensor stores
  (`Log.cpp`, `Sensor.cpp`).
- Self-update source: `--application-enable-self-update` adds a version-managed app whose
  files are the AppManager distribution for the current platform; `fp_SelfUpdate`
  (`Malterlib_Cloud_App_AppManager_SelfUpdate.cpp:17`) diff-copies them over the program
  directory.
- Framework facts: no existing docker/VZ code anywhere; `CProcessLaunchParams` is fully
  serializable; unix sockets supported (`ENetAddressType_Unix`); macOS frameworks link via
  `%Dependency "Virtualization" { !!PlatformIsDarwin true  Dependency { Type "Framework" } }`;
  ObjC++ goes in platform-guarded `.mm` files (Process module precedent).

## 3. Design Overview

### 3.1 Environments as first-class entities

New managed entity alongside applications, persisted in the state JSON under
`"Environments"` and manipulated through new CLI commands and remote API calls:

```
--environment-add --name <Name> --type <container|vm> [type-specific options]
--environment-change-settings --name <Name> [...]
--environment-remove --name <Name>
--environment-start / --environment-stop / --environment-restart --name <Name>
--environment-list
```

Remote API: `f_EnvironmentAdd/Remove/Start/Stop/ChangeSettings/GetEnvironments` added to
`CAppManagerInterface` (protocol bump, see §6), with permissions
`AppManager/Command/EnvironmentAdd` etc. following the existing permission scheme
(`Malterlib_Cloud_App_AppManager_AppManagerInterface.cpp:15-42`).

Environment settings:

| Setting | Type | Applies to | Meaning |
|---------|------|-----------|---------|
| `Type` | `Container` \| `VM` (\| `Local`, §3.6) | all | Environment kind |
| `ContainerRuntime` | `Default` \| `Docker` \| `AppleContainer` | Container | Runtime dialect; `Default` = docker on Linux, first available of docker/`container` on macOS |
| `AgentApplication` | string | all | Name of the self-update-style application that provides the agent binaries (§3.5), e.g. `SelfUpdate.Linux-arm64` |
| `AutoStart` | bool | all | Start environment at AppManager boot |
| `ContainerImage` | string | Container (required) | Base image reference |
| `ContainerNetwork` | string | Container | Network mode (default `host` on Linux docker; bridge + host-gateway on macOS docker; default vmnet network for Apple `container`) |
| `ContainerExtraMounts` / `ContainerExtraArguments` | map / vector | Container | Escape hatches |
| `MemoryLimit` / `CPULimit` | string / fp64 | Container + VM | `--memory`/`--cpus` or VZ memory/CPU-count |
| `VMImage` | string | VM (required) | Prepared guest bundle under `<Root>/VMImages/<name>/` |
| `VMBackend` | string | VM | Virtualization backend (default `Default` = platform choice; initially only `MacOSVirtualization`, §5.1) |

Runtime state per environment (not settings): agent connection status, agent HostID,
launch/ticket state, applications currently launched inside it. Modeled as a new
`CAppManagerActor::CEnvironment` struct analogous to `CApplication`
(`AppManager.h:154-273`), stored in a `TCMap<CStr, TCSharedPointer<CEnvironment>>`.

Applications reference an environment with one new app setting:

```
LaunchEnvironment: "" (default = host) | "<environment name>"
```

Validation: mutually exclusive with `LaunchInProcess` and `SelfUpdateSource`; referenced
environment must exist at launch time (launch fails with actionable status otherwise, so
settings can be staged first). Several applications may name the same environment.

### 3.2 The agent is the AppManager

Each running environment contains one AppManager instance in **agent mode**:

- Container: the AppManager binary is the container's main process
  (`AppManager --environment-agent ...`), executed from the agent application's directory
  mounted into the container (§3.5). One long-running container per environment; apps
  launched inside share it.
- macOS VM: the AppManager is installed as a LaunchDaemon in the guest during provisioning
  (§5.3) and runs at guest boot.

Agent mode is a startup mode of the existing daemon: when environment-agent configuration
is present (command-line/env/config file with host address + connection ticket), the
AppManager skips host-only subsystems (host monitor reboot control, encryption setup,
version-manager subscriptions, coordination) and exposes the environment-agent interface
(§3.3). It keeps: process launching, user/group provisioning
(`fsp_CreateApplicationUserGroup` and the macOS account code — refactored to be callable
outside the add/update pipeline), limits setup inside the guest, log capture.

**Refactoring prerequisite**: extract the launch-side machinery out of
`fp_LaunchAppInternal`'s external branch, `ApplicationManagement.cpp`'s
user/group/permission code, and `fp_RunUpdateScript`'s script-execution core into methods
usable both by the local paths and by the agent servicing remote launch/script requests.
This is the largest mechanical work item and should land as a behavior-neutral refactor
first (§7 Phase 1).

### 3.3 Host ↔ agent interface: distributed actors + tickets

New interface `CAppManagerEnvironmentInterface` (new files
`AppManager/Source/Malterlib_Cloud_App_AppManager_EnvironmentInterface.{h,cpp}`), following
the `CAppManagerCoordinationInterface` pattern (own namespace
`com.malterlib/Cloud/AppManagerEnvironment`, own protocol version enum, streamed structs).
Direction: the **agent publishes** the interface; the host holds a
`TCDistributedActor<CAppManagerEnvironmentInterface>` per running environment.

Connection bootstrap reuses the existing automatic ticket flow rather than inventing one:

- **Container**: the environment's agent container is launched with the *existing*
  `CDistributedAppInterfaceLaunchActor` — the `docker run`/`container run` CLI is just the
  launched process, attached stdio carries the same magic-line ticket handshake, and the
  env vars (`MalterlibDistributedAppInterfaceServerAddress`, `...RequestTicket`,
  `...LaunchID`) are passed with `--env`. The agent then registers like a distributed app
  but with delegated trust (`_bDelegateTrust`, already supported —
  `DistributedAppInterfaceLaunch.h:34`), so the host can subsequently discover its
  published `CAppManagerEnvironmentInterface` through the trust manager
  (`f_SubscribeTrustedActors`) filtered by the environment's LaunchID/HostID.
- **VM**: no attached stdio by default; attach a VZ serial console (host end = pipe pair)
  to the guest agent's stdio so the identical handshake works, or provision
  address + one-shot ticket via a file on the shared virtiofs volume. Decide during the VM
  phase; the serial-console route keeps one code path.

Interface methods (initial set):

```cpp
struct CAppManagerEnvironmentInterface : CActor
{
	// Agent-side services called by the host
	f_GetAgentInfo()                            -> platform, version, capabilities
	f_LaunchApplication(CEnvironmentLaunch)     -> launch app inside environment
	f_StopApplication(name, flags)              -> stop (grace + kill semantics as f_Stop)
	f_SubscribeApplicationState(functor)        -> Launched/LaunchFailed/Exited + status text
	                                               (mirrors EProcessLaunchState so the host
	                                               state-change handling is shared)
	f_RunScript(CEnvironmentScript)             -> run a bash script inside the environment
	                                               (update scripts; streamed output, exit
	                                               status; see below)
	f_EnsureUserGroup(user, group, options)     -> account provisioning inside guest
	f_Shutdown()                                -> graceful agent/environment stop
};
```

`CEnvironmentLaunch` carries the serialized launch description the host already computes
today: executable path relative to the (shared) app directory, parameters, working dir,
environment map incl. the *app's* ticket env vars, run-as user/group, limits. The launched
app's own distributed registration continues to go **directly to the host AppManager**
(same `FOnUseTicket` flow as today) — the agent only forwards stdout/stdin for the magic
ticket handshake as part of its launch handling, exactly as
`CDistributedAppInterfaceLaunchActor` does locally inside the guest.

**Update scripts run inside the environment.** The app's PreUpdate/PostUpdate/PostLaunch/
OnError scripts prepare the app's *runtime* environment (install packages, migrate data
with the app's own tools), so for an app with `LaunchEnvironment` set they must execute in
the guest, not on the host. `fp_RunUpdateScript`
(`Malterlib_Cloud_App_AppManager_UpdateScript.cpp:44`) is refactored so its core (resolve
script path against the app directory, launch the guest's bash, stream output into the
`[App/ScriptName]`-tagged log, map exit status to success/error) runs agent-side behind
`f_RunScript`; the host side keeps stage sequencing, version-info parameters/env, and error
handling, and merely dispatches locally or to the environment's agent. `CEnvironmentScript`
carries: script path (valid inside the guest thanks to the identical-path share), argument,
working dir, env map (version info), run-as user/group. Output is streamed back so host
logging is unchanged. Consequence for the update pipeline
(`Malterlib_Cloud_App_AppManager_UpdateProcess.cpp`): stages that run scripts require the
environment to be **running** — the update pipeline ensures/starts the app's environment
before `EUpdateStage_PreUpdateScript` and treats environment start failure as an update
failure at that stage. Host-infrastructure scripts (encryption open/close, limits setup,
self-update) remain host-side.

Host-side representation: extend `CApplication::m_ProcessLaunch` with a fourth variant
member holding the environment-launch handle (environment reference + remote subscription).
The state-change handling in `Launch.cpp:334-527` is reused by translating
`f_SubscribeApplicationState` events into the same code paths
(`fp_ScheduleRelaunchApp`, `fp_AppLaunchStateChanged`).

Failure semantics: if the agent connection drops (container died, VM crashed), the host
marks all applications launched in that environment as exited (drives the normal relaunch
scheduling) and restarts the environment per `AutoStart`/backoff policy — the environment
itself gets the same 10 s-delay retry treatment as apps (`fp_ScheduleRelaunchApp`
precedent).

### 3.4 Filesystem, users, and lifecycle mapping

| Concern | Container | macOS VM |
|---|---|---|
| App files | `<Root>/App` bind-mounted at the identical path when the environment container is created; app dirs, `.home`, `.tmp` visible unchanged | virtiofs share of `<Root>/App` at the identical guest path |
| Agent binary | Agent application directory (§3.5) mounted read-only; entrypoint executes from it | Installed into guest during provisioning; updated from the shared agent directory |
| Updates/backup/encryption | Unchanged — host-side file operations; the environment only executes. **Verification item**: mount propagation for encrypted app dirs mounted *after* environment start (bind-mount `rshared` on Linux; virtiofs submount behavior on VZ). If propagation is unavailable, either restart the environment on encryption open or mount per-app subdirectories | same |
| RunAsUser/Group | Agent creates users/groups inside the container (its own /etc/passwd namespace) via the refactored provisioning code | Agent uses the existing macOS account-creation code with correct permissions |
| Resource limits | Environment-level `--memory`/`--cpus` at container creation; per-app `CRegisterInfo::m_Resources_*` applied by the agent inside the guest (its existing `Limits.cpp` path) | VZ CPU/memory at VM config; per-app limits by agent |
| Update scripts (pre/post/…) | Run **inside the container** via the agent's `f_RunScript` (§3.3); update pipeline starts the environment before script stages | Run **inside the guest** via the agent |
| Stop app | Host → `f_StopApplication` → agent soft-stops process group (existing semantics) | same |
| Stop environment | Stop all contained apps, `f_Shutdown` the agent, then `docker stop` fallback / `container stop`; startup cleanup removes stale `mib-env-<name>` containers | agent shutdown, then VZ graceful stop, hard stop fallback |
| Exit/crash of app | Agent reports via `f_SubscribeApplicationState` → host relaunch scheduling unchanged | same |

### 3.5 Agent binaries via multi-platform self-update applications

Extend `--application-enable-self-update` (`CommandLine.cpp:463`), which today adds one
"SelfUpdate" application for the current platform:

- New option `Platforms` (list), e.g.
  `--application-enable-self-update Platforms=Linux-arm64,macOS-arm64`, creating one
  managed application per additional platform (naming: `SelfUpdate.<Platform>`), same
  version-manager application but pinned to that platform's version stream
  (`CVersionIDAndPlatform` already models platform-qualified versions).
- These apps have no executable to launch (like parent apps today, `"No exe"` path at
  `Launch.cpp:146`) — they exist to keep an up-to-date AppManager distribution for the
  platform on disk, flowing through the normal update pipeline (download, unpack, update
  scripts, coordinated updates).
- An environment's `AgentApplication` names one of these. Agent update policy: when the
  agent application updates, environments referencing it get flagged for restart (reuse the
  update-stage machinery so it can participate in coordinated updates later).
- Platform validation: reject platforms with no version available; on Linux hosts the
  host's own SelfUpdate app can double as the agent source for docker environments
  (same platform). Container environments on macOS hosts (docker or Apple `container`)
  always need a Linux-platform agent app (e.g. `SelfUpdate.Linux-arm64`).
- **Development note**: Linux is buildable on a macOS host — to produce the Linux agent
  binary during development, edit `BuildSystem/Default/UserSettings.MSettings` and set
  (uncommenting as needed) `SinglePlatform "Linux"` (plus `SingleArchitecture` /
  `SingleConfiguration` as desired), then build the AppManager target. For local testing
  the cross-built binary can populate the agent application directory directly, without a
  VersionManager round-trip.

### 3.6 A `Local` environment type for tests

Add a third environment type `Local`: the agent is launched as a plain child process on the
host (no isolation), using exactly the same launch/ticket/interface path as containers.
This exercises the entire delegation machinery (agent mode, environment interface, remote
launch, state forwarding, agent restart) in ordinary unit/integration tests without
docker/VZ present, and is the primary development vehicle for Phase 1.

## 4. Container Backends

All container control is explicit AppManager code driven through `CProcessLaunchActor`
(no Process-module changes):

- New file `Malterlib_Cloud_App_AppManager_Environment.cpp` — `CEnvironment` lifecycle
  (start/stop/restart, agent connection state machine, app-to-environment association,
  startup reconciliation/cleanup of stale containers).
- New file `Malterlib_Cloud_App_AppManager_EnvironmentContainer.cpp` — runtime dialect
  table (`docker` vs `container`), detection (`docker version` /
  `container system status`), argument building as pure functions
  (`fg_BuildEnvironmentRunArguments(...)` → unit-testable without any runtime), and the
  `docker stop`/`rm -f` control commands via `CProcessLaunchActor::fs_LaunchSimple`.
  The docker dialect is **platform-independent** (Docker Desktop/colima on macOS, docker on
  Linux) — only the networking defaults differ per host platform, selected via
  `ContainerRuntime`/`ContainerNetwork` (§3.1). This makes macOS dev machines the primary
  development/test vehicle for the docker backend before Linux-host validation.
- Environment container invocation (assembled, not hardcoded):
  `run --name mib-env-<Name> --rm --interactive --network <net> -v <Root>/App:<Root>/App -v <AgentAppDir>:<AgentAppDir>:ro [--memory][--cpus][extra] <image> <AgentAppDir>/AppManager --environment-agent ...`
  launched through `CDistributedAppInterfaceLaunchActor` for the ticket handshake (§3.3).
- Network reachability of the host's `wss://[<hostname>]/` address
  (`CDistributedAppActor::fp_GetLocalAddress`,
  `Malterlib_Concurrency_DistributedApp.cpp:318`) from inside the environment:
  - Linux docker, `--network host` (default): direct; add `--add-host <hostname>:127.0.0.1`
    if the name doesn't resolve in the container.
  - Linux docker, bridge: `--add-host <hostname>:host-gateway`.
  - macOS docker (Docker Desktop/colima): no host networking (docker runs in a Linux VM);
    bridge with `--add-host <hostname>:host-gateway` (maps to `host.docker.internal`'s
    gateway, supported by Docker Desktop and colima).
  - Apple `container`: containers sit on a vmnet subnet with the host reachable via the
    gateway; resolve the gateway at start and inject a hosts entry (**verification item**:
    `--add-host` equivalent in the `container` CLI; fallback is translating the address the
    host hands to the agent/apps).
  - **Verification item (highest risk, verify first)**: confirm the distributed-app
    websocket server binds an interface reachable from container/VM networks and that
    hostname-based trust works with injected hosts entries.

## 5. macOS VM Backend

### 5.1 New module `Malterlib/Virtualization`

New repository module (consumers pull it in with `MalterlibModules =+ "Virtualization"` —
already wired into the AppManager's `.MHeader`). It is designed as a **virtual actor
interface with pluggable backend implementations**, so further hypervisors (QEMU/KVM on
Linux, Hyper-V, cloud VMs) can be added later without touching consumers:

- **Interface**: `NVirtualization::CVirtualMachineActor : NConcurrency::CActor` — abstract
  actor with pure virtual methods (the `CDistributedAppInterfaceClient` precedent for
  virtual-interface actors):
  - `f_Start()`, `f_Stop()` (graceful guest shutdown request), `f_ForceStop()`
  - `f_GetState()` + state-change subscription (Stopped/Starting/Running/Stopping/Failed)
  - `f_ConnectConsole()` → host ends of the guest console (the agent stdio channel, §3.3)
  - `f_GetCapabilities()` → backend capabilities (supported guest OS types, shared
    folders, vsock, snapshots) so callers can validate configuration before start
- **Configuration**: backend-neutral `CVirtualMachineConfig` (CPU count, memory, disks,
  network mode, shared folders `tag → host path`, console, machine-identity storage
  location) plus a per-backend extension slot for settings that don't generalize.
- **Factory**: `fg_CreateVirtualMachine(backend, config) -> TCActor<CVirtualMachineActor>`
  with an `EVirtualizationBackend` enum (initially `Default`, `MacOSVirtualization`);
  `Default` resolves per host platform, and `fs_GetAvailableBackends()` reports what is
  usable on this host for validation/diagnostics.
- **First implementation**: `CVirtualMachineActor_MacOS` — ObjC++ wrapper around
  Virtualization.framework (`VZVirtualMachine`), in platform-guarded `.mm` files under
  `Source/Platform/` (Process-module convention). VZ delegate callbacks map onto
  futures/the state subscription — this async-by-construction fit is why VMs get a module
  while containers are just CLI calls.
- Build: `Malterlib_Virtualization.MHeader` with
  `%Dependency "Virtualization" { !!PlatformIsDarwin true  Dependency { Type "Framework" } }`;
  non-macOS builds still compile the interface + factory (backend reported unavailable),
  keeping consumers platform-clean.
- **Entitlement**: `com.apple.security.virtualization` is mandatory for the macOS backend,
  but it is **not** an Apple-managed entitlement — local builds work with ad-hoc signing
  and an entitlements plist
  (`codesign --entitlements Virtualization.entitlements --force -s - AppManager`), no
  Developer account/provisioning/notarization required. Decide via a Phase 4 spike whether
  to sign the AppManager itself (post-build codesign step) or delegate VM control to a
  small entitled helper executable; build-system task either way (entitlement plumbing in
  generator settings — no repo precedent). Restricted entitlements only enter the picture
  for bridged networking (`com.apple.vm.networking`, Apple-granted + provisioning
  profile) — not needed, the design uses NAT.
- macOS guests require Apple silicon hosts.

### 5.2 VM environments

`Malterlib_Cloud_App_AppManager_EnvironmentVM.cpp`: written **against the
`CVirtualMachineActor` interface only** — it obtains a `TCActor<CVirtualMachineActor>` per
running VM environment through `fg_CreateVirtualMachine` (backend from the optional
`VMBackend` environment setting, default `Default`), wires the guest agent's stdio through
`f_ConnectConsole` into the same `CDistributedAppInterfaceLaunchActor` handshake (§3.3),
shares `<Root>/App` and the agent application directory as shared folders, and maps
environment stop to agent shutdown + `f_Stop` with `f_ForceStop` fallback. Backend
availability (`fs_GetAvailableBackends`) drives launch-time validation, so no
hypervisor-specific `#ifdef`s are needed in the AppManager.
Guest networking: NAT; the agent writes the hosts entry for the host's hostname → NAT
gateway before connecting (part of agent-mode startup).

### 5.3 Guest image provisioning (separate deliverable)

CLI-driven provisioning (`--vm-image-create` or an external `mib`-style tool): install
macOS from IPSW via `VZMacOSInstaller` into `<Root>/VMImages/<name>/`, first-boot setup
(service user, install the agent LaunchDaemon pointing at the shared agent directory,
enable the serial-console agent stdio), snapshot as the reusable bundle. Distribution of
prepared bundles through VersionManager is a follow-up; initially bundles are provisioned
locally.

## 6. Settings, Protocol, CLI Plumbing (follows the `LaunchInProcess` precedent)

- App setting: `EApplicationSetting_LaunchEnvironment = DBit(29)` (include in
  `EApplicationSetting_NeedUpdateSettings`), `CStr m_LaunchEnvironment` in
  `CApplicationSettings` (`AppManager.h:107-152`); all seven `Settings.cpp` functions;
  persisted in `fp_UpdateApplicationJson` + read-back; wire field
  `TCOptional<CStr> m_LaunchEnvironment` in `CAppManagerInterface::CApplicationSettings`
  and `CStr` in `CApplicationInfo`, gated behind
  `EProtocolVersion_AddLaunchEnvironment = 0x11a`
  (`Cloud/Source/Malterlib_Cloud_AppManager.h:20-36`); CLI `--launch-environment` on add /
  change-settings.
- Environment entity: `CEnvironmentSettings` (+ parse/apply/diff/validate mirroring the app
  settings functions), persisted under `"Environments"` in the state JSON; new
  `CAppManagerInterface` calls `f_EnvironmentAdd/Remove/Start/Stop/ChangeSettings/
  f_GetEnvironments` + change-notification extension; permissions
  `AppManager/Command/Environment*` and `AppManager/Environment/<name>`; CLI commands per
  §3.1 in `CommandLine.cpp`.
- Self-update: `Platforms` option on `--application-enable-self-update`
  (`CommandLine.cpp:463`), plumbed into the add path with platform-qualified version
  selection.
- New interface `CAppManagerEnvironmentInterface` with its own protocol version enum
  (published by agents; not part of `CAppManagerInterface` versioning).
- `f_GetInstalled`/list output and the WebAppManager UI gain environment columns/status
  (UI in a polish phase).

## 7. Sequenced Work Breakdown

1. **Phase 0 — plumbing (no behavior change)**: app `LaunchEnvironment` setting +
   environment entity settings/persistence + CLI/remote API + permissions + protocol bump.
   Environments can be defined but only `Local` type is accepted for start (behind a debug
   flag until Phase 1 lands).
2. **Phase 1 — delegation machinery on `Local` environments**: behavior-neutral refactor of
   launch/user-provisioning/update-script code (§3.2); agent mode;
   `CAppManagerEnvironmentInterface` incl. `f_RunScript` and the update-pipeline
   ensure-environment-running step; ticket bootstrap via
   `CDistributedAppInterfaceLaunchActor`; host-side `CEnvironment` state machine +
   `m_ProcessLaunch` variant + shared state-change handling; agent-loss → app-relaunch
   semantics. Full test coverage with `Local` environments (§3.6, no container runtime
   needed), including update cycles whose scripts execute through the agent.
3. **Phase 2 — docker (develop on macOS, validate on Linux)**: dialect table + argument
   builder + reachability (bridge/host-gateway on macOS, host/bridge on Linux) +
   stale-container cleanup + `Platforms=` self-update extension (a Linux agent binary is
   required already for macOS docker development) + integration tests gated on docker
   availability, run on both host platforms.
4. **Phase 3 — Apple `container` on macOS**: second dialect in the existing table, vmnet
   gateway/hosts injection, `container system` availability detection, macOS integration
   tests (macOS 15+, macOS 26 recommended).
5. **Phase 4 — macOS VMs**: entitlement/signing spike; `Malterlib/Virtualization` module
   (virtual `CVirtualMachineActor` interface + factory + `CVirtualMachineActor_MacOS`
   backend, §5.1); `EnvironmentVM.cpp` against the interface; console handshake;
   provisioning tool; manual end-to-end checklist on Apple silicon.
6. **Phase 5 — polish**: environment resource statistics surfaced into sensors
   (`docker stats` / VZ), coordinated-update participation for agent updates, WebAppManager
   UI, documentation in `Cloud/Documentation`.

## 8. Testing

- **Unit (always run)**: argument-builder pure functions; runtime-detection output parsing;
  settings/environment parse/apply/diff/validate; JSON persistence round-trips; protocol
  streaming at old + new versions (existing patterns in
  `Cloud/Test/Test_Malterlib_Cloud_AppManager_*.cpp`).
- **Integration, no isolation runtime required**: full add→launch→register→crash→relaunch→
  stop→remove cycles against `Local` environments — this covers the agent mode, interface,
  ticket flow, and host state machine.
- **Integration, runtime-gated**: same cycles on docker (macOS dev machines and Linux CI —
  one dialect, both hosts) and Apple `container` (macOS dev machines), skipped cleanly when
  the runtime is absent; plus multi-app-per-environment, agent-kill recovery,
  stale-container cleanup.
- **VM**: agent/interface logic is already covered by `Local`; VZ-specific paths verified
  by a manual checklist first, automated only if an entitled Apple-silicon CI host exists.
- Run with `MalterlibBuildShowProgress=false ./mib test --paths '["Malterlib/Cloud/AppManager*"]'`.

## 9. Risks and Open Questions

- **Server reachability / hostname trust from guest networks** (§4) — verify first, in
  Phase 2 spike form, before committing to per-backend network defaults.
- **Mount propagation** for encryption volumes opened after environment start (§3.4) —
  affects whether encrypted apps can share long-running environments or force an
  environment restart on encryption open. On macOS docker the app directory additionally
  crosses the Docker Desktop/colima file-sharing layer (VirtioFS) — `<Root>/App` must be
  inside the configured shared paths, and propagation/performance need verification there
  too.
- **Apple `container` CLI parity** (attached-stdio semantics, `--add-host` equivalent,
  exit-code propagation) — keep the dialect table data-driven so differences stay
  contained; needs verification on macOS 26.
- **Entitlement/signing pipeline** for Virtualization.framework — no repo precedent, but
  low risk: ad-hoc signing with the entitlements plist suffices for local development
  (§5.1); the work is build-system integration, and the entitled-helper fallback bounds it
  further.
- **Agent trust scope**: delegated trust gives the agent the ability to request tickets on
  behalf of apps; review what permissions an environment agent's identity should carry
  (likely a restricted variant of the AppManager-to-AppManager trust).
- **Container image contract**: minimum requirements for `ContainerImage` (libc
  compatibility with the agent binary, **bash present** for update scripts, writable /tmp);
  document rather than solve; consider publishing a reference base image later.
- **Coordinated updates across environments**: agent-application updates restarting
  environments must not violate update-group coordination — initially restart-on-update is
  immediate and documented; proper staging integrates with `CoordinatedUpdate.cpp` in
  Phase 5.
