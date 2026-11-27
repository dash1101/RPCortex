// The roots the board trusts for HTTPS, built into the image (ca_certs.pem,
// turned into a table by tools_cacerts.py). Each is DER in flash, with where its
// subject Name sits, so a handshake can find the issuer it needs without
// parsing the others.
#ifndef RPC_STOCKCA_H
#define RPC_STOCKCA_H

#include <stdint.h>

struct StockCa {
    const uint8_t *der;
    uint16_t       len;
    uint16_t       subj_off;    // the subject Name, as a whole DER SEQUENCE
    uint16_t       subj_len;
};

extern const StockCa  stock_ca[];
extern const unsigned stock_ca_count;

#endif
