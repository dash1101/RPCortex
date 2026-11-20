// `update`, between its two halves: update.cpp (writing an image over the
// firmware, from-file, rollback, the report at boot) and update_net.cpp (check
// and install over the network, which an image without Wi-Fi leaves out and
// net_off.cpp stands in for).
#ifndef RPC_UPDATE_H
#define RPC_UPDATE_H

// Where a download is staged as a file, and where the manifest it came with is
// kept. update_report_boot removes both once the new firmware has started.
#define IMAGE_PATH    "/os/update.bin"
#define MANIFEST_PATH "/os/update.json"

// Stage `path` and copy it over the firmware. Does not return when it works.
bool update_apply_file(const char *path, const char *to_version, bool at_boot);

// update check, and update install [--force] [--yes].
int update_check(bool quiet);
int update_online(bool force, bool yes);

#endif  // RPC_UPDATE_H
