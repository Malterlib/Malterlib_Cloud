# AppManager Launch Environments

The AppManager can launch the applications it manages in a launch environment instead of directly on the host: a
container or a virtual machine that isolates the application from the host and from other applications, or a local
environment that runs the same machinery on the host without isolation. The isolation of containers has limits that
the threat model describes: they share the default network of their runtime unless configured otherwise, a network of
their own still reaches the services of the host unless its firewall blocks them, and Colima environments share one
VM. An application names its environment in its launch environment setting; without one it runs on the host as
before. Several applications can share an environment.

Environments are managed entities of their own, next to applications. They are added, changed, started, stopped,
restarted and removed through the command line and the remote API, with permissions per command and per environment.
Assigning an application to an environment runs it there, so it needs access to the environment as well as to the
application. Naming an agent or parent application for an environment needs access to that application; the default
agent needs none: the AppManager's own distribution for the guest platform of a container or VM, and a copy of the
running AppManager for a Local environment. The settings that reach
past the environment boundary need the host access permission of the AppManager: extra mounts and extra runtime
arguments of containers, container networks other than the runtime's defaults (bridge, none and default), like the
host network, the network of another container or a named network, and the OS dependencies of a Local environment,
which install on the host. Settings of the host that restart environments to apply them, like the Colima and VM
settings, need access to each environment they restart. So do adding a Colima environment and changing the extra
mounts, parent application or runtime of one, which change the directories that the Colima VM shares: the next start
of a Colima environment applies them by restarting the Colima VM with every Colima environment running in it. Opening
the encryption of the parent application of a Colima environment changes the shares as well.

## Environment types

- **Local** runs the environment agent as a child process on the host. It isolates nothing and exists to exercise
  the agent machinery without a container runtime or a hypervisor, in tests and for development. Its agent runs with
  the privileges of the AppManager.
- **Container** runs the agent as the main process of a persistent Linux container, one container per environment.
  The runtime is Docker on Linux hosts. On macOS hosts it is the Apple container runtime, the default, which runs
  each container in a lightweight VM, or Colima, which runs Docker in a VM that the AppManager owns. A stopped
  container is reused on the next start, so changes inside it, like OS updates, survive restarts; a change of the
  settings the container was created with replaces it with a new one, after an attempt to pull the newest image,
  which falls back to the image the host already has. Containers use the default network of the runtime unless
  they are configured for another, so on a Docker host they share its default bridge with the other containers
  there; see the threat model. The guest host names of VM guests, and of Docker containers that don't share the host
  name of the host or of another container, like those on the host network, are made from the name of the host computer and the environment, apart
  from the container names that `docker` commands take: a container takes a new computer name when it is next created
  and a VM guest when the host next installs its agent. The Apple container runtime names the guest host after the
  container, since it keeps the agent from renaming it.
- **VM** runs a macOS guest through Virtualization.framework on Apple silicon hosts with macOS 26 or later, which
  the isolated network of the guest needs. The guest boots from an image
  bundle that the AppManager creates from a macOS restore image, which also holds first-boot provisioning: an
  account with Remote Login that the host uses to install the agent as a daemon in the guest. The provisioning needs a
  macOS 27 host and restore image. Without it, set with an empty `--provision-username`, the guest is set up by hand
  with `--environment-window`, and the environment starts only with an agent it installed while the image had
  provisioning. A VM can also be shown
  in a window: `--environment-window` hosts it in the command line client, and the VM window setting starts a
  separate window host when a graphical session is available. Each VM gets a network of its own that reaches the
  internet over IPv4, but not the host, the networks the host is connected to, other private and link-local networks
  or other guests; only the host can open connections to the guest. Until the host knows its own networks, the guest
  can only use DHCP and DNS and reply to connections the host opened. Networks with public addresses that the host
  only routes to, like static routes or the prefixes of a split-tunnel VPN, stay reachable like the rest of the
  internet.

The types differ mainly in where application files live, how the agent connects and how the environment stops. A
VM stops through a shutdown that a connected agent starts in the guest, since a host-side stop request can turn into
a confirmation dialog in a guest with a logged-in user; a VM that the AppManager runs gets a host-side stop when the
guest doesn't shut down.

## The environment agent

The agent inside an environment is the AppManager itself, running in agent mode. It launches, supervises and
relaunches the applications of its environment, creates their accounts, runs their scripts, installs their OS
dependencies and reports their status to the host. It skips the host-only parts of the AppManager, like the key
manager, version manager and coordination with other AppManagers. Agent executables for other platforms than the
host's are installed as applications of their own, one per platform, and kept current by the normal update pipeline;
an environment names the application that provides its agent.

Host and agent talk through distributed actors:

- The host publishes an interface that agents call to register, report application states, request connection
  tickets for their applications and request restarts of their environment.
- The agent gives the host an interface of its own when it registers, which the host calls to configure the agent,
  launch and stop applications, run scripts, stage and apply application files and shut the environment down. The
  agent doesn't publish it, so only its host can call it.

An agent first connects with a ticket, which makes the host trust it; after that it reconnects without one and
registers with the launch ID the host keeps for it. Local and container agents get the ticket through the same
handshake on their standard streams that every launched distributed application uses; a VM agent has no attached
streams, and the host writes its connection settings and ticket into the guest over SSH. Applications in an
environment register directly with the host AppManager, and the agent requests the tickets that make the host trust
them.

A VM guest can't reach the host over its network, so the host forwards a virtio socket port to the environment
listen, a Unix domain socket on the host, and the agent forwards a socket at the same path in the guest to that
port. The agent and the applications of the environment connect to that socket, and the tickets name the same
path on both sides.

Both interfaces have protocol versions of their own. Host and agent negotiate the newest version both support when
they connect, and the newer side checks the negotiated version before it calls anything the older side doesn't
have. Each side refuses peers older than its minimum version, and both interfaces have a single version so far, so
host and agent have to come from releases with the same interface versions.

## Application storage

An application keeps its files in one of two places, chosen by its environment:

- **Host storage**: the application directory on the host, as for applications that run on the host. Local
  environments use it. Updates and backups work on the host directory.
- **Environment-managed storage**: data storage of the environment, where the agent creates the application
  directories. Containers and VMs use it. The host stages the files of an update in host storage of its own and
  transfers them into the environment, where the agent applies them; application backups aren't supported.

The data storage of a container on a Linux host is a directory of the environment storage on the host, mounted
read-write into the container, so it shares the file system of the host applications. On macOS hosts it is a disk
image that the Colima VM mounts, or a volume of the Apple container runtime, and a VM has a data disk. The host
creates the data storage and deletes it, with the environment stopped; installs and updates reach inside it only
through the agent. Only the
runtime that created a volume reaches it, so the runtime of an environment doesn't change while its volume exists.
A VM also boots the writable system disk of its image bundle, which keeps what earlier guests wrote to it: an image
bundle serves one environment at a time, and an environment that must not see what another guest wrote gets a fresh
one.

Each container also mounts a read-only agent directory with the agent executable and its deployment settings, which
only the host writes. The host installs a new agent beside the running one and switches to it with a rename, so the
container never sees a partial update; the container runs the new agent when it restarts. The AppManager state of
the agent, like its trust database, logs and state, lives in the data storage.

The storage of an application can't change while it is installed, so an application moves only between the host and
Local environments, which share host storage. Files that a container or VM could change never return to the host or
reach another environment, other than through an image bundle that the next VM boots.

The AppManager keeps environment storage in `.Environment` and VM images in `.VMImages` directories, in its root
directory and in application directories, names that no application can take. An environment can have a parent
application. Its storage, and the images of its VMs, then live in the directory of that application, which on Linux
hosts can be encrypted: the environment starts only once the encryption is open, and stops before it closes. The
encryption covers the data storage that the container mounts, but not the writable file system of the container
itself or the volumes that its image declares, which the runtime keeps in its own storage; a container that must not
keep writes outside its data storage runs with `--container-read-only`, an image without volumes and no writable
extra mounts, also none in the extra runtime arguments. The output of the container, which holds the log of its
agent, stays in the logs of the runtime outside the encryption, like with the `json-file` logging driver of Docker. A
container with
extra mounts from `--container-extra-mounts` in the directory of an encrypted application also stops before that
encryption closes and starts only once it is open; mounts in the extra runtime arguments aren't tracked. The parent
application can't be removed while an environment stores data in it. The backups of every application leave out the
`.Environment` and `.VMImages` directories at every depth of its directory, which covers those of its child
applications, also of removed ones, the storage that a removed environment keeps and images no environment uses yet.
A backup include rule of the application that maps these directories to another destination doesn't back them up
reliably: a new backup takes them, but later changes to them don't reach the backup. Like any rule that maps files to
another destination, it fails the backup when another include rule also takes those files. It also copies data that guests
write into the backup, so such a rule must not map them where guest data must stay out of host backups. On macOS
hosts a container environment with a parent application needs the Colima runtime, since the Apple container runtime
keeps its volumes outside the directory of the application.

Removing an application or an environment keeps the data in environment-managed storage, which an environment
added again with the same name, parent application and container runtime uses. A package added again with the same
name is refused unless it is added with `--force-install` while its files are there. The data is deleted only when the
removal is given `--delete-application-data`, which for an environment deletes its storage with the data of its
container or VM, but not the image bundle of a VM with its system disk, and a removal that can't delete it fails and
keeps the application or environment, so it can be retried; a forced application removal only warns, but keeps an
application in a container or VM that doesn't stop. Only the agent can delete the data of an application, so deleting
it starts its container or VM, also one that was stopped manually, which the last deletion stops again.

## Lifecycle

- An environment starts when an application in it needs to run, or at AppManager startup when it starts
  automatically. Environments whose parent application is encrypted start once the encryption opens.
- A manual stop wins over automatic starts: an environment stopped manually stays stopped, also for application
  starts and update scripts that need it, until it is started or restarted manually. Deleting application data and
  opening the VM window boot it without ending the manual stop. A stop or restart waits until the
  operations in the environment are idle, but for a container or VM at most an hour for the operations that only
  wait for its guest, like update scripts and file applies in it. Closing the encryption of a parent application, or
  of an application that the extra mounts of a container are in, stops those environments without waiting for them to
  be idle, so work in them that must finish needs the environment stopped first.
- Stopping an environment stops its applications through the agent first. When the agent is lost, the host clears
  the launches in the environment, stops it and retries the applications later.
- Moving an application into or out of a Local environment stops it, changes its settings and launches it at its
  new place.
- Updates of applications in environment-managed storage are staged inside the environment and applied by the agent.
  The pre and post update, post launch and error scripts of an application in an environment run inside the
  environment, through the agent, as the application's accounts there; only the AppManager's own infrastructure
  scripts run on the host.
- An update of an agent application restarts the environments that use it once they are idle.

## OS dependencies and patching

Applications declare the OS packages they need per OS family, and environments with `--os-dependencies` as a plain
list of package names for the OS of their guest. The AppManager installs them on the host for applications whose
executable runs there, and the agent installs them inside the environment before it launches the applications, with
the package manager of the platform: apt, dnf or microdnf, zypper, apk or pacman on Linux and Homebrew on macOS. Dependencies
that match an OS without one fail the installation. On Arch Linux, which supports no partial upgrades, installing a
missing package upgrades the whole system. The dependencies of the AppManager's own distribution are the tools of the
host monitor of its agents, so the host doesn't install them for a self update source, and an agent that can't install them still
launches the applications of its environment.

Agents in containers and VMs run their own host monitor, configured from the host's automatic update settings: they
patch the environment's OS and ask the host to restart the environment when an update needs it. The agent of a
read-only container only monitors. Only Linux guests
are patched; a macOS VM guest logs that it doesn't install the updates. An update an agent reports holds back host
reboots and environment stops for at most an hour, and the host refuses updates that start once it stops the
environment, reboots or shuts down. The sensors of applications in a container or VM hold back automatic host reboots and
automatic environment restarts for at most an hour in all, however the guests report them. A manual environment
restart doesn't wait for them, while a manual host reboot is refused as long as anything holds it back, guests
included. The AppManager reboots only Linux hosts, so on macOS hosts what this section says about host reboots
doesn't apply, while environment restarts are held back the same way. Local agents share the host OS, which the host
AppManager patches and reboots, so they get no automatic update settings.

## Threat model

- **Trusted, out of scope**: the host, its administrator, the AppManager process, its state and its configuration.
  Authenticated API callers are trusted within their permissions; a missing or wrong permission check is a bug.
- **Application packages and their scripts are trusted**: the AppManager doesn't defend against an application's own
  content or update scripts, like links an application plants in its own directory. The scripts of an application in
  a container or VM environment run inside that environment, never on the host.
- **A container or VM environment is a boundary.** The agent runs inside it and is trusted no more than anything else
  in the environment. The host validates everything the agent sends, like paths, names, IDs and sizes, doesn't let
  the agent choose host paths or host accounts, and gives the environment no access to the host or to other
  environments beyond what it needs. A VM guest reaches the host only through its environment listen, over a limited
  number of connections at a time; its network filter drops what it sends to the host, the networks the host is
  connected to, other private and link-local networks and other guests, except DHCP and DNS from its gateway and the
  replies to connections the host opened, and it drops everything else while the networks of the host are unknown.
  A guest reaches the globally routable addresses outside the networks the host is connected to, also on networks
  the host only routes to, so a host that must keep guests away from such internal networks blocks them with a
  firewall. The host applies no application updates inside storage a guest can write, and reads it only for the
  backups of an application whose include rules map its environment storage elsewhere; writable extra mounts that the
  host allows expose their files to both. What a guest reports about its applications, like resource needs,
  connection tickets and config files, doesn't affect the host beyond that environment, except that their sensors
  and the OS updates the agent reports hold back host reboots within the limits above. A compromised root inside a
  guest must not reach the host or other environments.
- The Colima VM is shared by all Colima environments of the host. It shares only their read-only agent directories,
  their data directories and the extra mounts the host allowed them, so a container that escapes into the VM reaches
  those of every Colima environment and the other containers in the VM, but no other host files. Colima
  environments are therefore not isolated from each other; that would take a VM per environment.
- Containers on the default network of their runtime reach the other containers on it, like those of the other
  Docker environments of a Linux host. Separating them takes a network of their own for each, which an administrator
  with the host access permission configures. A network of its own separates a Docker container from its peers, not
  from the host: it still reaches the services of the host and the local network that its network routes to, which
  takes firewall rules of the host to block. A Docker agent on a Linux host reaches its host through a mounted Unix
  socket, so a container whose applications need no network can use the `none` network there; Colima and Apple
  container agents connect over the network.
- A Local environment isn't a boundary: it runs on the host with the privileges of the AppManager.

## Not implemented yet

- Container end-to-end tests on a Linux CI host, and the full application cycles on the container runtimes.
- Network isolation for Colima and Apple containers, whose agents reach the environment listen over the container
  network.
- A web UI for environments; the command line and the remote API are the interface.
- Change notifications for environments; the remote API returns snapshots.
- Agent updates that take part in coordinated updates of update groups.
- Resource accounting per environment, and per-application resource limits inside environments; the host monitors
  of agents already report the disk space and memory of their guests.
- Distribution of prepared VM image bundles through the version manager.
- Hypervisors other than Virtualization.framework.
