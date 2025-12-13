# Malterlib Cloud Infrastructure on AWS

This guide describes the setup for a minimal Malterlib Cloud infrastructure on AWS, including redundancy considerations for critical components.

## Overview

A minimal Malterlib Cloud deployment requires:
- **AppManager** - Manages applications on each host
- **VersionManager** - Stores and distributes application versions
- **MalterlibCloud CLI** - Administration tool for managing the infrastructure

Optional but recommended:
- **KeyManager** - Root encryption manager for block-level encryption (Linux only)
- **SecretsManager** - Secure storage for passwords and private keys
- **CloudManager** - Monitoring and status reporting
- **BackupManager** - Backup management (pre-release)

## Architecture Diagram

```
                           ┌────────────────────────────────────────────────────────────┐
                           │                        AWS VPC                             │
                           │                                                            │
                           │  ┌───────────────────────────────────────────────────────┐ │
                           │  │              MANUALLY MANAGED HOSTS                   │ │
   ┌───────────────┐       │  │  ┌─────────────────┐       ┌─────────────────┐        │ │
   │ Admin Host    │       │  │  │ KeyManager (A)  │◄─────►│ KeyManager (B)  │        │ │
   │               │       │  │  │ (AZ-A)          │ Sync  │ (AZ-B)          │        │ │
   │ MalterlibCloud│───────┼──┼──│                 │       │                 │        │ │
   │ CLI           │       │  │  └────────▲────────┘       └────────▲────────┘        │ │
   └───────────────┘       │  │           │                         │                 │ │
                           │  │           │ Key requests            │                 │ │
                           │  │  ┌────────┴─────────────────────────┴────────┐        │ │
                           │  │  │ Management Host                           │        │ │
                           │  │  │  ├─ AppManager (manual)                   │        │ │
                           │  │  │  ├─ CloudManager                          │        │ │
                           │  │  │  ├─ VersionManager                        │        │ │
                           │  │  │  └─ SecretsManager                        │        │ │
                           │  │  └───────────────────┬───────────────────────┘        │ │
                           │  └──────────────────────┼────────────────────────────────┘ │
                           │                         │ Manages                          │
                           │                         ▼                                  │
                           │  ┌───────────────────────────────────────────────────────┐ │
                           │  │              CLOUDMANAGER-MANAGED HOSTS               │ │
                           │  │  ┌───────────┐  ┌───────────┐  ┌───────────┐          │ │
                           │  │  │AppManager │  │AppManager │  │AppManager │  ...     │ │
                           │  │  │+ App X    │  │+ App Y    │  │+ App Z    │          │ │
                           │  │  └───────────┘  └───────────┘  └───────────┘          │ │
                           │  └───────────────────────────────────────────────────────┘ │
                           │                                                            │
                           └────────────────────────────────────────────────────────────┘
```

## AWS Resources Required

### 1. VPC and Networking

| Resource | Configuration |
|----------|--------------|
| VPC | /16 CIDR block (e.g., 10.0.0.0/16) |
| Subnets | At least 2 private subnets in different AZs |
| NAT Gateway | For outbound internet access (updates) |
| Security Groups | Restrict access to management ports |

**Security Group Rules for Malterlib Cloud:**

| Port | Protocol | Source | Purpose |
|------|----------|--------|---------|
| 1392 | TCP | VPC CIDR | VersionManager WebSocket |
| 1393 | TCP | VPC CIDR | KeyManager WebSocket |
| 1394 | TCP | VPC CIDR | SecretsManager WebSocket |
| 22 | TCP | Admin IPs | SSH access |

### 2. EC2 Instances

#### Management Host (Primary)
- **Instance Type**: t3.medium or larger
- **OS**: Ubuntu 22.04 LTS (recommended) or Amazon Linux 2
- **Storage**:
  - Root: 20 GB gp3
  - Data: 100+ GB gp3 (for VersionManager application storage)
- **IAM Role**: Permissions for S3 (backups), CloudWatch (monitoring)

#### Redundant KeyManager Host (Optional)
- **Instance Type**: t3.small
- **OS**: Same as primary
- **Storage**: 20 GB gp3 (KeyDatabase is small)
- **Note**: Runs as an active synchronized instance, not a cold standby

#### Application Hosts
- **Instance Type**: Based on application requirements
- **OS**: Linux recommended for block-level encryption support
- **Storage**: Based on application needs

### 3. Storage

| Resource | Purpose | Configuration |
|----------|---------|---------------|
| EBS Volumes | KeyManager database | Encrypted, gp3 |
| S3 Bucket | Version artifacts backup | Versioning enabled, encryption |

## Installation Steps

### Step 1: Prepare the Management Host

```bash
# Connect to EC2 instance
ssh -i key.pem ubuntu@<management-host-ip>

# Create Malterlib directory
sudo mkdir /M
cd /M

# Download and extract AppManager (Linux)
sudo tar --no-same-owner -xvf ~/AppManager.tar.gz

# Install AppManager as daemon
sudo ./AppManager --daemon-add
```

### Step 2: Install VersionManager

```bash
cd /M

# Add VersionManager as managed application
sudo ./AppManager --application-add \
    --name VersionManager \
    --from-file \
    --update-tags '["Production"]' \
    --update-branches '["*"]' \
    ~/VersionManager.tar.gz

# Configure VersionManager to listen on network
# Replace VERSION_MANAGER_HOST with your EC2 private DNS or Elastic IP
sudo ./App/VersionManager/VersionManager \
    --trust-listen-add "wss://VERSION_MANAGER_HOST:1392/"
```

### Step 3: Install KeyManager (Optional but Recommended)

```bash
cd /M

# Add KeyManager with auto-update disabled
# Tags/branches specify which versions are acceptable for manual updates
sudo ./AppManager --application-add \
    --name KeyManager \
    --from-file \
    --no-auto-update \
    --update-tags '["Production"]' \
    --update-branches '["*"]' \
    ~/KeyManager.tar.gz

# Configure KeyManager to listen
sudo ./App/KeyManager/KeyManager \
    --trust-listen-add "wss://KEYMANAGER_HOST:1393/"

# IMPORTANT: After restart, you must provide the password
sudo ./App/KeyManager/KeyManager --provide-password
```

**Note**: KeyManager uses `--no-auto-update` to disable automatic updates. As the root of encryption for the infrastructure, updates should be performed manually after careful review and testing. The `--update-tags` and `--update-branches` options specify which versions are acceptable when you do update manually.

### Step 4: Set Up Trust Relationships

```bash
# Generate ticket for MalterlibCloud CLI access to VersionManager
sudo ./App/VersionManager/VersionManager \
    --trust-generate-ticket \
    --permissions '["Application/WriteAll", "Application/TagAll", "Application/ReadAll"]'

# On admin workstation, add connection
./MalterlibCloud --trust-connection-add \
    --trusted-namespaces '["com.malterlib/Cloud/VersionManager"]'
# Paste the ticket when prompted
```

### Step 5: Configure AppManager Self-Update

```bash
cd /M

# Connect AppManager to VersionManager
sudo ./AppManager --trust-connection-add \
    --trusted-namespaces '["com.malterlib/Cloud/VersionManager"]' \
    `sudo ./App/VersionManager/VersionManager \
        --trust-generate-ticket \
        --permissions '["Application/ReadAll"]'`

# Enable self-update
sudo ./AppManager --application-enable-self-update \
    --update-tags '["Production"]' \
    --update-branches '["*"]'
```

## Trust Hierarchy and Host Design

Understanding the trust hierarchy is essential for designing your infrastructure and optimizing costs.

### Trust Model

```
┌─────────────────────────────────────────────────────────────────┐
│                    MANUALLY MANAGED                             │
│  ┌─────────────┐         ┌─────────────┐                       │
│  │ KeyManager  │◄───────►│ KeyManager  │  (sync with each other)│
│  │   Host A    │         │   Host B    │                       │
│  └─────────────┘         └─────────────┘                       │
│         ▲                       ▲                               │
│         │ Key requests          │                               │
│         │                       │                               │
│  ┌──────┴───────────────────────┴──────┐                       │
│  │           CloudManager              │  (manually managed)    │
│  │  + VersionManager                   │                       │
│  │  + SecretsManager                   │                       │
│  └──────────────┬──────────────────────┘                       │
└─────────────────┼───────────────────────────────────────────────┘
                  │ Manages
                  ▼
┌─────────────────────────────────────────────────────────────────┐
│                  CLOUDMANAGER-MANAGED                           │
│  ┌───────────┐  ┌───────────┐  ┌───────────┐                   │
│  │ AppManager│  │ AppManager│  │ AppManager│  ...              │
│  │ + App X   │  │ + App Y   │  │ + App Z   │                   │
│  └───────────┘  └───────────┘  └───────────┘                   │
└─────────────────────────────────────────────────────────────────┘
```

### Trust Rules

| Component | Managed By | Trusts | Notes |
|-----------|------------|--------|-------|
| KeyManager | Manual only | Other KeyManagers | Root of encryption, never auto-updated |
| CloudManager | Manual only | KeyManager | Central management, never auto-updated |
| VersionManager | Manual only | KeyManager | Can colocate with CloudManager |
| SecretsManager | Manual only | KeyManager | Can colocate with CloudManager |
| All other apps | CloudManager | CloudManager, KeyManager | Auto-updated via VersionManager |

### Host Consolidation Options

Different configurations trade off cost vs. isolation:

#### Option 1: Maximum Isolation (High Security Production)
- **Host A**: KeyManager only
- **Host B**: KeyManager only (redundancy)
- **Host C**: CloudManager only
- **Host D**: VersionManager only
- **Host E**: SecretsManager only
- **Hosts F+**: Application hosts (one app per host)

*Cost: Highest, but strongest security boundaries - compromise of one host doesn't affect others*

#### Option 2: Moderate Consolidation (Standard Production)
- **Host A**: KeyManager only
- **Host B**: KeyManager only (redundancy)
- **Host C**: CloudManager + VersionManager + SecretsManager
- **Hosts D+**: Application hosts

*Cost: Medium, core management consolidated but KeyManagers isolated*

#### Option 3: Minimal Setup (Development/Staging)
- **Host A**: KeyManager + CloudManager + VersionManager + SecretsManager
- **Hosts B+**: Application hosts (optional second KeyManager on one app host)

*Cost: Lowest, suitable for non-production environments*

**Important**: KeyManagers and CloudManager cannot be on hosts managed by CloudManager - they must be manually managed to avoid circular dependencies and maintain the trust hierarchy.

### Storage Considerations

Each application managed by AppManager can have its own storage volume, optionally with block-level encryption (Linux only). This provides data isolation even when multiple apps share a host.

#### Storage Options

| Option | Description | Use Case |
|--------|-------------|----------|
| Shared volume | Apps share host's root volume | Dev/test, non-sensitive data |
| Separate volumes | Each app gets dedicated EBS volume | Data isolation, independent snapshots |
| Encrypted volumes | Separate volume with block-level encryption | Sensitive data, compliance requirements |

#### Encrypted Volume Setup

When an application is configured with encryption, AppManager:
1. Creates a block device file for the application
2. Requests an encryption key from KeyManager (keyed to HostID)
3. Mounts the encrypted volume at `App/AppName/`

```bash
# Add application with encrypted storage (Linux only)
sudo ./AppManager --application-add \
    --name MyApp \
    --encryption-storage /M/App/MyApp/storage.img \
    --encryption-file-system ext4 \
    --update-tags '["Production"]' \
    --update-branches '["*"]'
```

The `--encryption-storage` option specifies the file or block device for encrypted storage. The `--encryption-file-system` option specifies the filesystem type (`ext4`, `xfs`, or `zfs`).

**Note**: Encrypted volumes require a connection to KeyManager. The AppManager will continue running if KeyManager becomes unavailable, but encrypted volumes cannot be mounted until KeyManager connectivity is restored (e.g., after AppManager restart).

## KeyManager Redundancy

The KeyManager is critical infrastructure - it stores encryption keys for all managed applications. KeyManager supports built-in synchronization between instances, making redundancy setup straightforward.

### Setting Up Synchronized KeyManagers

Deploy two KeyManager instances in different Availability Zones. They automatically synchronize keys between them, so both instances are always up-to-date and can serve requests.

**Step 1: Install KeyManager on both hosts**

```bash
# On Host A (AZ-A) and Host B (AZ-B)
cd /M

# Install AppManager first if not already present
sudo tar --no-same-owner -xvf ~/AppManager.tar.gz
sudo ./AppManager --daemon-add

# Add KeyManager through AppManager (auto-update disabled for this critical component)
sudo ./AppManager --application-add \
    --name KeyManager \
    --from-file \
    --no-auto-update \
    --update-tags '["Production"]' \
    --update-branches '["*"]' \
    ~/KeyManager.tar.gz

# Configure to listen on network
sudo ./App/KeyManager/KeyManager \
    --trust-listen-add "wss://KEYMANAGER_HOST:1393/"
```

**Step 2: Establish trust and synchronization between KeyManagers**

```bash
# On Host A: Generate ticket for Host B
sudo ./App/KeyManager/KeyManager \
    --trust-generate-ticket \
    --permissions '["KeySync"]'

# On Host B: Add connection to Host A (paste ticket when prompted)
sudo ./App/KeyManager/KeyManager \
    --trust-connection-add \
    --trusted-namespaces '["com.malterlib/Cloud/KeyManager"]'

# Repeat in reverse direction for bidirectional sync
# On Host B: Generate ticket for Host A
sudo ./App/KeyManager/KeyManager \
    --trust-generate-ticket \
    --permissions '["KeySync"]'

# On Host A: Add connection to Host B
sudo ./App/KeyManager/KeyManager \
    --trust-connection-add \
    --trusted-namespaces '["com.malterlib/Cloud/KeyManager"]'
```

**Step 3: Provide password on both instances**

```bash
# On both Host A and Host B (use the same password)
sudo ./App/KeyManager/KeyManager --provide-password
```

**Step 4: Connect AppManagers to both KeyManagers**

AppManagers can connect to multiple KeyManagers for redundancy:

```bash
# On application hosts: add connections to both KeyManagers
sudo ./AppManager --trust-connection-add \
    --trusted-namespaces '["com.malterlib/Cloud/KeyManager"]' \
    `sudo ssh hostA ./App/KeyManager/KeyManager --trust-generate-ticket --permissions '["Key/Read"]'`

sudo ./AppManager --trust-connection-add \
    --trusted-namespaces '["com.malterlib/Cloud/KeyManager"]' \
    `sudo ssh hostB ./App/KeyManager/KeyManager --trust-generate-ticket --permissions '["Key/Read"]'`
```

### How Synchronization Works

- Both KeyManager instances are **active** and can serve key requests
- When a new key is created on one instance, it is automatically synchronized to the other
- AppManagers can connect to either or both KeyManagers
- If one KeyManager goes down, the other continues serving requests
- When the failed instance comes back online, it automatically synchronizes any missed keys

### Important Notes

- **Same password required**: All synchronized KeyManagers must use the same database password
- **After restart**: You must provide the password on each KeyManager after it restarts: `./KeyManager --provide-password`
- **Network connectivity**: KeyManagers must be able to reach each other for synchronization
- **Manual updates only**: KeyManager uses `--no-auto-update` to disable automatic updates. Update both instances manually after testing, coordinating to avoid simultaneous restarts

## Security Best Practices

### 1. Network Isolation

- Deploy management components in private subnets
- Use VPC endpoints for AWS services (S3, Secrets Manager)
- Enable VPC Flow Logs for audit

### 2. Encryption

- Enable EBS encryption for all volumes
- Use KMS customer-managed keys
- Enable S3 bucket encryption with KMS

### 3. Access Control

- Use IAM roles for EC2 instances (no access keys)
- Enable MFA for console access
- Use Session Manager instead of SSH where possible

### 4. KeyManager Password Management

**Critical**: The KeyManager password is the root of trust for all encryption.

- Store in AWS Secrets Manager with restricted access
- Enable secret rotation alerts
- Document secure access procedure for authorized personnel
- **Synchronized KeyManagers must use the same password** - ensure this is coordinated when setting up redundancy

### 5. Monitoring

```bash
# CloudWatch Alarms to configure:
# - KeyManager instance health
# - VersionManager disk space
# - Failed SSH attempts
# - Unusual API activity
```

## Cost Estimates

### Option 3: Minimal Setup (Dev/Staging)
Single management host with all core components.

| Resource | Specification | Monthly Cost (approx) |
|----------|--------------|----------------------|
| EC2 Management Host | t3.medium (KeyManager + CloudManager + VersionManager + SecretsManager) | $30 |
| EBS Volumes | 120 GB gp3 | $10 |
| S3 Storage | 50 GB with versioning | $2 |
| NAT Gateway | 1 instance + data | $35 |
| **Total** | | **~$77/month** |

### Option 2: Moderate Consolidation (Standard Production)
KeyManagers isolated, management components consolidated.

| Resource | Specification | Monthly Cost (approx) |
|----------|--------------|----------------------|
| EC2 KeyManager Host A | t3.small | $15 |
| EC2 KeyManager Host B | t3.small | $15 |
| EC2 Management Host | t3.medium (CloudManager + VersionManager + SecretsManager) | $30 |
| EBS Volumes | 160 GB gp3 | $13 |
| S3 Storage | 50 GB with versioning | $2 |
| NAT Gateway | 1 instance + data | $35 |
| **Total** | | **~$110/month** |

### Option 1: Maximum Isolation (High Security Production)
One application per host for strongest security boundaries.

| Resource | Specification | Monthly Cost (approx) |
|----------|--------------|----------------------|
| EC2 KeyManager Host A | t3.small | $15 |
| EC2 KeyManager Host B | t3.small | $15 |
| EC2 CloudManager Host | t3.small | $15 |
| EC2 VersionManager Host | t3.medium | $30 |
| EC2 SecretsManager Host | t3.small | $15 |
| EBS Volumes | 200 GB gp3 total | $16 |
| S3 Storage | 50 GB with versioning | $2 |
| NAT Gateway | 1 instance + data | $35 |
| **Total** | | **~$143/month** |

*Note: Use Reserved Instances or Savings Plans for 30-60% cost reduction on all options.*

## Adding Application Hosts

For each new host that needs to run managed applications:

```bash
# On the new host
sudo mkdir /M && cd /M
sudo tar --no-same-owner -xvf ~/AppManager.tar.gz
sudo ./AppManager --daemon-add

# Generate ticket on VersionManager host
sudo ./App/VersionManager/VersionManager \
    --trust-generate-ticket \
    --permissions '["Application/ReadAll"]'

# On new host: add connection (paste ticket when prompted)
sudo ./AppManager --trust-connection-add \
    --trusted-namespaces '["com.malterlib/Cloud/VersionManager"]'

# Enable self-update
sudo ./AppManager --application-enable-self-update \
    --update-tags '["Production"]' \
    --update-branches '["*"]'

# Add your application
sudo ./AppManager --application-add \
    --update-tags '["Production"]' \
    --update-branches '["*"]' \
    YOUR_APPLICATION_NAME
```

## Environment Workflow Tags

Configure deployment pipelines using version tags:

| Tag | Environment | Who Can Set |
|-----|-------------|-------------|
| `Test` | Development/Test | CI/CD pipeline |
| `Staging` | Pre-production | DevOps team |
| `Production` | Production | Release manager only |

```bash
# Tag a version for production deployment
./MalterlibCloud --version-manager-change-tags \
    --application MyApp \
    --version "main/1.2.3" \
    --add '["Production"]'
```

## Troubleshooting

### KeyManager won't start after reboot
```bash
# Provide the unlock password
sudo ./App/KeyManager/KeyManager --provide-password
```

### Application not updating
```bash
# Check AppManager logs
sudo tail -f /M/Log/AppManager.log

# Verify trust connection
sudo ./AppManager --trust-connection-list
```

### VersionManager connection refused
```bash
# Check listening status
sudo ./App/VersionManager/VersionManager --trust-listen-list

# Verify security group allows port 1392
```

## References

- [Malterlib Cloud Infrastructure PDF](MalterlibCloudInfrastructure.pdf)
- [Malterlib Distributed Apps Documentation](../../../Documentation/)
- [AWS Well-Architected Framework](https://aws.amazon.com/architecture/well-architected/)
