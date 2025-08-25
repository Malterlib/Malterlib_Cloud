# CLAUDE.md - Cloud Module

This file provides guidance to Claude Code (claude.ai/code) when working with the Cloud module in the Malterlib framework.

## Module Overview

The Cloud module provides a comprehensive distributed application management framework for deploying, managing, and monitoring cloud-based applications. It includes tools for application lifecycle management, secure secrets storage, version control, backup systems, and distributed debugging capabilities.

Key features include:
- **Application Management** - Deploy, update, and monitor distributed applications
- **Version Management** - Track and distribute application versions across environments
- **Secrets Management** - Secure storage and distribution of secrets and credentials
- **Backup Systems** - Automated backup and recovery for application data
- **Cloud Integration** - OpenStack Swift and cloud API integration
- **Debug Infrastructure** - Crash dump collection and distributed debugging
- **Network Tunneling** - Secure network tunnels for remote access
- **Key Management** - Encryption key management and distribution
- **Host Monitoring** - System health monitoring and automatic recovery

## Architecture

### Core Components

#### Application Management
- **CAppManager** - Main application lifecycle manager
- **CAppManagerInterface** - Actor interface for application management
- **CAppDistributionManager** - Manages application distribution across hosts
- **CCloudClient** - Client application for cloud management operations

#### Version Control
- **CVersionManager** - Manages application versions and updates
- **CVersionInfo** - Version metadata and dependency tracking
- **CVersionIDAndPlatform** - Platform-specific version identification

#### Secrets and Security
- **CSecretsManager** - Secure secrets storage and retrieval
- **CKeyManager** - Encryption key management
- **CKeyManagerServer** - Key distribution server
- **CKeyManagerDatabase_EncryptedFile** - Encrypted file storage

#### Backup and Recovery
- **CBackupManager** - Centralized backup management
- **CBackupManagerClient** - Client-side backup operations
- **CBackupManagerDownload** - Backup retrieval operations
- **CBackupInstance** - Individual backup management

#### Cloud Infrastructure
- **CCloudManager** - Central cloud orchestration
- **CCloudAPIManager** - Cloud API gateway (OpenStack Swift)
- **CHostMonitor** - Host health monitoring
- **CNetworkTunnels** - Secure tunneling infrastruceture

#### Debug and Monitoring
- **CDebugManager** - Crash dump and debug asset management
- **CDebugManagerClient** - Client-side debug collection
- **CDebugManagerHelper** - Debug utilities

### Design Principles

1. **Distributed First** - All components designed for distributed deployment
2. **Security by Default** - Encryption and authentication built-in
3. **Resilient Operations** - Automatic recovery and retry mechanisms
4. **Versioned Updates** - Safe, coordinated application updates
5. **Observable Systems** - Comprehensive logging and monitoring
6. **Minimal Downtime** - Rolling updates and graceful transitions
7. **Platform Agnostic** - Support for macOS, Linux, and Windows

## File Organization

```
Cloud/
├── Apps/                          # Manager applications
│   ├── AppDistributionManager/    # Application distribution
│   ├── AppManager/                # Application lifecycle management
│   ├── BackupManager/             # Backup operations
│   ├── CloudAPIManager/           # Cloud API gateway
│   ├── CloudClient/               # Client application
│   ├── CloudManager/              # Cloud orchestration
│   ├── DebugManager/              # Debug infrastructure
│   ├── DebugManagerClient/        # Debug client
│   ├── KeyManager/                # Key management
│   ├── SecretsManager/            # Secrets storage
│   ├── TestApp/                   # Test application
│   ├── TunnelProxyManager/        # Network tunneling
│   └── VersionManager/            # Version control
├── Source/                        # Core library implementations
├── Include/Mib/Cloud/             # Public interfaces
├── Test/                          # Unit tests
└── Documentation/                 # Module documentation
```

## Common Patterns

### Distributed Actor Communication

```cpp
// Get distributed actor reference
auto Actor = co_await fg_GetDistributedActor<CMyInterface>();

// Call with timeout
auto Result = co_await Actor.f_CallActor(&CMyInterface::f_Method, Params).f_Timeout(30.0, "Operation timed out");

// Handle connection failures
auto Result = co_await Actor.f_CallActor(&CMyInterface::f_Method).f_Wrap();
if (!Result)
	DConErrOut("Failed: {}\n", Result.f_GetExceptionStr());
```

## Testing

```bash
# Run all cloud tests
/opt/Deploy/Tests/RunAllTests --paths '["Malterlib/Cloud*"]'

# Run specific test suites
/opt/Deploy/Tests/RunAllTests --paths '["Malterlib/Cloud/AppManager*"]'
/opt/Deploy/Tests/RunAllTests --paths '["Malterlib/Cloud/SecretsManager*"]'
/opt/Deploy/Tests/RunAllTests --paths '["Malterlib/Cloud/BackupManager*"]'
```

## Dependencies

The Cloud module depends on:
- **Core** - Basic types and utilities
- **Concurrency** - Actor system and distributed computing
- **Network** - Network communication
- **File** - File system operations
- **Cryptography** - Encryption and authentication
- **Database** - Persistent storage
- **Process** - Process management
- **Web** - HTTP/REST communication
- **Encoding** - JSON/Binary encoding
- **Compression** - Data compression
- **Container** - Data structures
- **Storage** - Memory management

## Common Pitfalls

1. **Uncoordinated Updates** - Always use coordination for multi-host updates
2. **Plain Text Secrets** - Never store secrets unencrypted
3. **Missing Error Handling** - Always handle network failures
4. **Resource Leaks** - Ensure proper cleanup of connections and subscriptions
5. **Version Mismatches** - Check protocol versions before operations
6. **Blocking Operations** - Use async operations for all I/O
7. **Missing Authentication** - Always authenticate distributed operations
8. **Inadequate Monitoring** - Set up proper alerting for production
9. **No Backup Strategy** - Implement regular backups for critical data
10. **Ignoring Rate Limits** - Respect API rate limits and implement backoff

### Performance Optimization

- Use batch operations for multiple secrets/versions
- Enable compression for large file transfers
- Implement caching for frequently accessed secrets
- Use connection pooling for distributed actors
- Configure appropriate timeouts for operations
- Monitor and tune thread pool sizes
- Use incremental backups for large datasets
- Implement rate limiting for API calls
