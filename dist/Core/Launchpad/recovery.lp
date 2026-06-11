# RPCortex Pulsar — recovery & diagnostic command registry
# Kept separate from system.lp so these tools still load if system.lp is damaged.
# Format: command_name:/path/to/file.py:function_name
fscheck:/Core/Launchpad/sys_recovery.py:fscheck
diag:/Core/Launchpad/sys_recovery.py:diag
logdump:/Core/Launchpad/sys_recovery.py:logdump
regreset:/Core/Launchpad/sys_recovery.py:regreset
pkgdisable:/Core/Launchpad/sys_recovery.py:pkgdisable
pkgenable:/Core/Launchpad/sys_recovery.py:pkgenable
