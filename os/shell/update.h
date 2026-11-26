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

#if RPC_AB
// A/B (RP2350): the firmware an update replaced stays in the other partition.
// The record of it (partition, length, SHA-256, version) and going back to it.
void update_ab_forget(void);
// True when a previous firmware is recorded for the other partition; with
// `verify`, only when the bytes there still match it.
bool update_ab_prev(int *part, char *ver, uint32_t vcap, bool verify);
// Restart into it. Does not return when it works.
bool update_ab_switch_back(bool at_boot);
#endif

// update check, and update install [--force] [--yes].
int update_check(bool quiet);
int update_online(bool force, bool yes);

#endif  // RPC_UPDATE_H
