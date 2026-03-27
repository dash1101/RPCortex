# RPCortex Nebula — Launchpad system command registry
# Format: command_name:/path/to/file.py:function_name

# ---------------------------------------------------------------------------
# Filesystem  (sys_fs.py)
# ---------------------------------------------------------------------------
ls:/Core/Launchpad/sys_fs.py:ls
dir:/Core/Launchpad/sys_fs.py:ls
cd:/Core/Launchpad/sys_fs.py:cd
chdir:/Core/Launchpad/sys_fs.py:cd
pwd:/Core/Launchpad/sys_fs.py:pwd
touch:/Core/Launchpad/sys_fs.py:touch
write:/Core/Launchpad/sys_fs.py:touch
mkdir:/Core/Launchpad/sys_fs.py:mkdir
rm:/Core/Launchpad/sys_fs.py:delete
del:/Core/Launchpad/sys_fs.py:delete
delete:/Core/Launchpad/sys_fs.py:delete
rmdir:/Core/Launchpad/sys_fs.py:delete
read:/Core/Launchpad/sys_fs.py:read
open:/Core/Launchpad/sys_fs.py:read
view:/Core/Launchpad/sys_fs.py:read
cat:/Core/Launchpad/sys_fs.py:read
head:/Core/Launchpad/sys_fs.py:head
tail:/Core/Launchpad/sys_fs.py:tail
exec:/Core/Launchpad/sys_fs.py:execute
rename:/Core/Launchpad/sys_fs.py:rename
ren:/Core/Launchpad/sys_fs.py:rename
mv:/Core/Launchpad/sys_fs.py:move
move:/Core/Launchpad/sys_fs.py:move
cp:/Core/Launchpad/sys_fs.py:copy
copy:/Core/Launchpad/sys_fs.py:copy
df:/Core/Launchpad/sys_fs.py:diskfree
tree:/Core/Launchpad/sys_fs.py:tree

# ---------------------------------------------------------------------------
# System info / control  (sys_sys.py)
# ---------------------------------------------------------------------------
reboot:/Core/Launchpad/sys_sys.py:reboot
sreboot:/Core/Launchpad/sys_sys.py:sreboot
softreset:/Core/Launchpad/sys_sys.py:sreboot
sysinfo:/Core/Launchpad/sys_sys.py:sysinfo
meminfo:/Core/Launchpad/sys_sys.py:meminfo
uptime:/Core/Launchpad/sys_sys.py:uptime
date:/Core/Launchpad/sys_sys.py:date
ver:/Core/Launchpad/sys_sys.py:ver
version:/Core/Launchpad/sys_sys.py:ver
clear:/Core/Launchpad/sys_sys.py:clear
cls:/Core/Launchpad/sys_sys.py:clear
pulse:/Core/Launchpad/sys_sys.py:pulse
bench:/Core/Launchpad/sys_sys.py:bench
fetch:/Core/Launchpad/sys_sys.py:fetch
neofetch:/Core/Launchpad/sys_sys.py:fetch
edit:/Core/Launchpad/sys_sys.py:edit
nano:/Core/Launchpad/sys_sys.py:edit
vi:/Core/Launchpad/sys_sys.py:edit
env:/Core/Launchpad/sys_sys.py:env
reg:/Core/Launchpad/sys_sys.py:reg
freeup:/Core/Launchpad/sys_sys.py:freeup
gc:/Core/Launchpad/sys_sys.py:freeup
settings:/Core/Launchpad/settings.py:settings
factoryreset:/Core/Launchpad/sys_sys.py:factoryreset
reinstall:/Core/Launchpad/sys_sys.py:reinstall
update:/Core/Launchpad/sys_sys.py:update

# ---------------------------------------------------------------------------
# Network downloads  (sys_net.py)
# ---------------------------------------------------------------------------
wget:/Core/Launchpad/sys_net.py:wget
runurl:/Core/Launchpad/sys_net.py:runurl
run:/Core/Launchpad/sys_net.py:runurl
ping:/Core/Launchpad/sys_net.py:ping
nslookup:/Core/Launchpad/sys_net.py:nslookup
ns:/Core/Launchpad/sys_net.py:nslookup
curl:/Core/Launchpad/sys_net.py:curl

# ---------------------------------------------------------------------------
# User management  (sys_user.py)
# ---------------------------------------------------------------------------
whoami:/Core/Launchpad/sys_user.py:whoami
mkacct:/Core/Launchpad/sys_user.py:mkacct
rmuser:/Core/Launchpad/sys_user.py:rmuser
chpswd:/Core/Launchpad/sys_user.py:chpswd
logout:/Core/Launchpad/sys_user.py:logout
exit:/Core/Launchpad/sys_user.py:exit

# ---------------------------------------------------------------------------
# Misc  (now in sys_sys.py)
# ---------------------------------------------------------------------------
help:/Core/Launchpad/sys_sys.py:help
echo:/Core/Launchpad/sys_sys.py:say
print:/Core/Launchpad/sys_sys.py:say
history:/Core/Launchpad/sys_sys.py:history
