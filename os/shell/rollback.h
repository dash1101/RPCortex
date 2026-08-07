#pragma once
#include <stdint.h>

// Going back to the firmware that was working.
//
// There is one staging slot, and an update fills it with the NEW image — so at
// the moment the old firmware is overwritten there is nowhere raw left to put a
// copy of it. The only place a spare image can live is the filesystem, which is
// why this is a file rather than a second slot.
//
// That is also why the restore is two steps rather than one: the copier cannot
// read a file (it runs from RAM with the filesystem's code erased out from under
// it), so a rollback stages the saved image back into the slot first, exactly
// like an update does, and then applies it. Same path, same checks, same
// reboot — the only difference is which image is in the slot.

#define ROLLBACK_IMG "/os/rollback.img"
#define ROLLBACK_CFG "/os/rollback.cfg"

struct RollbackInfo {
    char     ver[24];        // the version this image is
    char     board[24];      // and the board it was built for
    uint32_t size;           // bytes, and the .img must match exactly
    uint32_t reserve;        // the flash layout it assumes
    char     sha[65];        // of the image, checked before it is staged
};

// Save the RUNNING firmware, so there is something to go back to. Called before
// an update applies, and by hand. Returns false if there is no room, which is
// not fatal to the update — it just means no safety net for that one.
bool rollback_capture(bool announce);

// What is saved, if anything usable is. False when there is nothing, when the
// pair disagrees, or when the image was built for another board or layout.
bool rollback_read(RollbackInfo *out);

// Drop the saved image and reclaim the space.
void rollback_forget(void);

// Put the saved image back. Does not return when it works: the device reboots
// into the restored firmware. `at_boot` is the automatic path, where the
// scheduler is not running yet and nobody is watching the console.
bool rollback_apply(bool at_boot);

// The automatic attempt, from kboot, when boots keep failing. Returns false if
// there was nothing to restore, and the caller falls through to rebuilding the
// filesystem as it did before.
bool rollback_try_at_boot(void);

// `update rollback` and its options. Lives under `update` rather than beside it
// because it is the same operation pointed the other way, and somebody looking
// for it will look there.
int rollback_command(int argc, char **argv);
