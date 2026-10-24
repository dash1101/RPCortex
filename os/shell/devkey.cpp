#include "devkey.h"
#include "lock.h"
#include "logring.h"
#include "sha256.h"
#include "storage.h"

#include "pico/rand.h"
#include "pico/unique_id.h"

#include <string.h>

static RpcLock g_lock;
static uint8_t g_key[32];
static bool    g_have;

static bool all_zero(const uint8_t *p, size_t n) {
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) acc |= p[i];
    return acc == 0;
}

bool devkey_get(uint8_t key[32]) {
    // One task makes the key. Two racing to do it would each seal under their own
    // and the loser's value could never be opened again.
    LockGuard guard(&g_lock);

    if (!g_have) {
        uint8_t k[32];
        uint32_t n = storage_read_file(DEVKEY_PATH, k, sizeof(k));
        if (n == sizeof(k) && !all_zero(k, sizeof(k))) {
            memcpy(g_key, k, sizeof(k));
            g_have = true;
        } else {
            // Random bytes from the hardware generator, hashed together with the
            // board's own ID so two devices that somehow drew the same bytes would
            // still differ.
            uint8_t seed[8 * 4 + 8];
            for (int i = 0; i < 4; i++) {
                uint64_t r = get_rand_64();
                memcpy(seed + i * 8, &r, 8);
            }
            pico_unique_board_id_t id;
            pico_get_unique_board_id(&id);
            memcpy(seed + 32, id.id, 8);
            sha256(seed, sizeof(seed), k);
            memset(seed, 0, sizeof(seed));

            // Durable before it is used: read it back, since a key that was
            // sealed under and then not saved takes every value with it.
            uint8_t check[32];
            if (!storage_write_file(DEVKEY_PATH, k, sizeof(k)) ||
                storage_read_file(DEVKEY_PATH, check, sizeof(check)) != sizeof(check) ||
                memcmp(check, k, sizeof(k)) != 0) {
                log_add(LOG_K_WARN, "[Secret] could not save the device key; secrets stay as they were");
                memset(k, 0, sizeof(k)); memset(check, 0, sizeof(check));
                return false;
            }
            memcpy(g_key, k, sizeof(k));
            g_have = true;
            log_add(LOG_K_INFO, "[Secret] created the device key");
            memset(check, 0, sizeof(check));
        }
        memset(k, 0, sizeof(k));
    }
    memcpy(key, g_key, sizeof(g_key));
    return true;
}
