// One transaction at a time on an I2C block.
//
// The Nova D1's display, its clock and its NFC reader share I2C0. The display is flushed by
// the package's GUI task through fw_i2c_write, and a PN532 exchange runs on whichever task asked
// for it, so two tasks can be inside the SDK's blocking transfer calls on the same block at the
// same moment. The SDK's calls are not safe to overlap: the second one's bytes end up in the
// first one's transfer, and the symptom is a corrupt frame on one side and a failed read on the
// other, never a crash.
//
// So every transaction on a bus takes this first. It guards ONE transfer, not a conversation:
// a PN532 command is several of them and the display may slot a page in between, which the chip
// does not mind because each transfer ends in a STOP. A change of bus speed is a thing that must
// never land inside somebody else's transfer, so it takes the lock too.
//
// The lock is a plain RpcLock, which already marks its holder as not safe to terminate while it
// holds it (lock_try enters a critical section), so a task killed inside a transfer cannot leave
// the bus owned by nobody.
#ifndef RPC_I2CLOCK_H
#define RPC_I2CLOCK_H

#include "lock.h"

// One lock per block, two blocks. The function-local static is one object across every file that
// calls this (C++17 inline), which is the whole point.
inline RpcLock *i2c_txn_lock(unsigned bus) {
    static RpcLock locks[2];
    return &locks[bus & 1u];
}

// Scope guard: `I2cTxn t(bus);` for the length of one transfer.
struct I2cTxn {
    LockGuard g;
    explicit I2cTxn(unsigned bus) : g(i2c_txn_lock(bus)) {}
};

#endif  // RPC_I2CLOCK_H
