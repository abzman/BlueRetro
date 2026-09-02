/*
 * Copyright (c) 2026, jjtcm
 * SPDX-License-Identifier: Apache-2.0
 *
 * LE Secure Connections (SC) pairing support for BlueRetro's SMP.
 *
 * All buffers follow the Linux kernel's LE (little-endian) convention:
 * public keys, DHKey, nonces, confirms, addresses and DHKey Checks are kept
 * in the same byte order as they appear on the wire (SMP PDUs). No byte
 * reversal is done anywhere; f4/f5/f6 messages are built exactly like
 * net/bluetooth/smp.c. This is what SDL/HIDAPI and most stack peers expect.
 *
 * Verified against kernel-equivalent test vectors (see comments).
 *
 * Crypto goes through PSA Crypto: ESP-IDF 6.x ships Mbed TLS 4 / TF-PSA-Crypto,
 * where the legacy mbedtls_aes/ecp/ecdh APIs are no longer public. Needs
 * CONFIG_MBEDTLS_CMAC_C, _ECP_C, _ECDH_C and _ECP_DP_SECP256R1_ENABLED,
 * all on by default in ESP-IDF 6.1.
 */

#include <stdio.h>
#include <string.h>
#include <esp_random.h>
#include <psa/crypto.h>
#include "bluetooth/host.h"
#include "bluetooth/mon.h"
#include "smp_sc.h"

/* Per-device SC pairing context. Only one pairing happens at a time in
 * practice; keyed per device id for safety. */
struct smp_sc_ctx {
    bool active;
    uint8_t priv[32];      /* our P-256 private scalar (LE wire order; PSA wants BE, converted at the boundary) */
    uint8_t pub_x[32];     /* our public key X (LE wire order) */
    uint8_t pub_y[32];     /* our public key Y (LE wire order) */
    uint8_t peer_x[32];    /* peer public key X (LE wire order) */
    uint8_t peer_y[32];    /* peer public key Y (LE wire order) */
    uint8_t dhkey[32];     /* ECDH shared secret X coord (LE wire order) */
    uint8_t na[16];        /* our (initiator/master) nonce (LE) */
    uint8_t nb[16];        /* peer (slave) nonce (LE) */
    uint8_t mackey[16];    /* f5 MacKey (LE) */
    uint8_t ltk[16];       /* f5 LTK (LE) */
    uint8_t eown[16];      /* our confirm Ea (LE) */
    uint8_t epeer[16];     /* peer confirm - raw bytes as received */
    /* IO capabilities from the pairing request/response PDUs (LE raw) */
    uint8_t iocap_a[3];
    uint8_t iocap_b[3];
    bt_addr_le_t local;    /* our identity */
    bt_addr_le_t remote;   /* peer identity (LE wire order as stored) */
};

static struct smp_sc_ctx smp_sc[BT_MAX_DEV] = {0};
/* SC is only requested after a peer refused legacy pairing; one attempt per
 * connection. Kept outside the context, which smp_sc_begin() clears. */
static bool smp_sc_attempted[BT_MAX_DEV] = {0};

static struct smp_sc_ctx *get_ctx(struct bt_dev *device) {
    return &smp_sc[device->ids.id];
}

/* ---------------------------------------------------------------- */
/* AES-CMAC (RFC 4493) through PSA                                   */
/* ---------------------------------------------------------------- */
static int aes_cmac128(const uint8_t key[16], const uint8_t *msg, size_t len, uint8_t out[16]) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    psa_status_t status;
    size_t out_len = 0;

    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return status;
    }
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_CMAC);

    status = psa_import_key(&attr, key, 16, &key_id);
    if (status == PSA_SUCCESS) {
        status = psa_mac_compute(key_id, PSA_ALG_CMAC, msg, len, out, 16, &out_len);
        psa_destroy_key(key_id);
    }
    psa_reset_key_attributes(&attr);
    if (status == PSA_SUCCESS && out_len != 16) {
        status = PSA_ERROR_GENERIC_ERROR;
    }
    return status;
}

/* ---------------------------------------------------------------- */
/* SC crypto functions                                              */
/* ---------------------------------------------------------------- */
static const uint8_t sc_salt[16] = {0x6C,0x88,0x83,0x91,0xAA,0xF5,0xA5,0x38,
                                    0x60,0x37,0x0B,0xDB,0x5A,0x60,0x83,0xBE};
static const uint8_t sc_btle[4]  = {0x62, 0x74, 0x6c, 0x65}; /* "btle" */
static const uint8_t sc_length[2] = {0x01, 0x00};             /* 256 BE */

/* Reverse helper for converting LE<->BE at the message boundary */
static void sc_rev(uint8_t *dst, const uint8_t *src, int n) {
    for (int i = 0; i < n; i++) {
        dst[i] = src[n - 1 - i];
    }
}

/* f4(U,V,X,z) = AES-CMAC_X(U||V||z) (65 bytes). Computes with BE
 * inputs and reverses on the wire; we store LE so convert at the boundary. */
static int sc_f4(const uint8_t u_le[32], const uint8_t v_le[32],
                 const uint8_t x_le[16], uint8_t z, uint8_t out_le[16]) {
    uint8_t buf[65];
    uint8_t u_be[32], v_be[32], x_be[16], out_be[16];
    sc_rev(u_be, u_le, 32);
    sc_rev(v_be, v_le, 32);
    sc_rev(x_be, x_le, 16);
    memcpy(buf, u_be, 32);
    memcpy(buf + 32, v_be, 32);
    buf[64] = z;
    int ret = aes_cmac128(x_be, buf, sizeof(buf), out_be);
    sc_rev(out_le, out_be, 16);
    return ret;
}

/* f5(W,N1,N2,A1,A2) -> [MacKey | LTK], layout:
 *   m = counter(1) || "btle"(4) || N1 || N2 || A1 || A2 || length(2)
 * values are BE internally, wire is LE. */
static int sc_f5(const uint8_t w_le[32], const uint8_t n1_le[16], const uint8_t n2_le[16],
                 const uint8_t a1_le[7], const uint8_t a2_le[7], uint8_t out_le[32]) {
    uint8_t t[16], buf[53];
    uint8_t w_be[32], n1_be[16], n2_be[16], a1_be[7], a2_be[7], f5out[32];

    sc_rev(w_be, w_le, 32);
    sc_rev(n1_be, n1_le, 16);
    sc_rev(n2_be, n2_le, 16);
    sc_rev(a1_be, a1_le, 7);
    sc_rev(a2_be, a2_le, 7);

    int ret;

    /* T = AES-CMAC(SALT, W) BE */
    ret = aes_cmac128(sc_salt, w_be, 32, t);
    if (ret) {
        return ret;
    }

    buf[0] = 0; /* counter 0 -> MacKey */
    memcpy(buf + 1, sc_btle, 4);
    memcpy(buf + 5, n1_be, 16);
    memcpy(buf + 21, n2_be, 16);
    memcpy(buf + 37, a1_be, 7);
    memcpy(buf + 44, a2_be, 7);
    memcpy(buf + 51, sc_length, 2);
    ret = aes_cmac128(t, buf, 53, f5out); /* MacKey BE */
    if (ret) {
        return ret;
    }

    buf[0] = 1; /* counter 1 -> LTK */
    ret = aes_cmac128(t, buf, 53, f5out + 16); /* LTK BE */
    if (ret) {
        return ret;
    }

    /* wire is LE - reverse each 16-byte half separately (so MacKey/LTK
     * halves are not swapped) */
    sc_rev(out_le, f5out, 16);
    sc_rev(out_le + 16, f5out + 16, 16);
    return 0;
}

/* f6(W,N1,N2,R,IOcap,A1,A2) = AES-CMAC_W(N1||N2||R||IOcap||A1||A2) */
static int sc_f6(const uint8_t w_le[16], const uint8_t n1_le[16], const uint8_t n2_le[16],
                  const uint8_t r_le[16], const uint8_t io_cap[3],
                  const uint8_t a1_le[7], const uint8_t a2_le[7], uint8_t out_le[16]) {
    uint8_t buf[65], w_be[16], n1_be[16], n2_be[16], r_be[16], a1_be[7], a2_be[7], out_be[16];

    sc_rev(w_be, w_le, 16);
    sc_rev(n1_be, n1_le, 16);
    sc_rev(n2_be, n2_le, 16);
    sc_rev(r_be, r_le, 16);
    sc_rev(a1_be, a1_le, 7);
    sc_rev(a2_be, a2_le, 7);

    memcpy(buf, n1_be, 16);
    memcpy(buf + 16, n2_be, 16);
    memcpy(buf + 32, r_be, 16);
    memcpy(buf + 48, io_cap, 3);
    memcpy(buf + 51, a1_be, 7);
    memcpy(buf + 58, a2_be, 7);
    int ret = aes_cmac128(w_be, buf, sizeof(buf), out_be);

    sc_rev(out_le, out_be, 16);
    return ret;
}

/* ---------------------------------------------------------------- */
/* ECDH P-256 through PSA (big-endian API - convert at the boundary) */
/* ---------------------------------------------------------------- */
static void smp_sc_reverse(uint8_t *dst, const uint8_t *src, int len) {
    for (int i = 0; i < len; i++) {
        dst[i] = src[len - 1 - i];
    }
}

static void smp_sc_ecc_attr(psa_key_attributes_t *attr, psa_key_usage_t usage) {
    psa_set_key_type(attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(attr, 256);
    psa_set_key_usage_flags(attr, usage);
    psa_set_key_algorithm(attr, PSA_ALG_ECDH);
}

/* Generate key pair. Outputs priv/pub in LE wire order (PSA exports the
 * scalar big-endian and the public key as 0x04 || X || Y big-endian). */
static int smp_sc_keygen(uint8_t priv_le[32], uint8_t px_le[32], uint8_t py_le[32]) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    psa_status_t status;
    uint8_t priv_be[32], pub[65];
    size_t len = 0;

    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return status;
    }
    smp_sc_ecc_attr(&attr, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    status = psa_generate_key(&attr, &key_id);
    if (status == PSA_SUCCESS) {
        status = psa_export_key(key_id, priv_be, sizeof(priv_be), &len);
        if (status == PSA_SUCCESS && len != sizeof(priv_be)) {
            status = PSA_ERROR_GENERIC_ERROR;
        }
        if (status == PSA_SUCCESS) {
            status = psa_export_public_key(key_id, pub, sizeof(pub), &len);
            if (status == PSA_SUCCESS && (len != sizeof(pub) || pub[0] != 0x04)) {
                status = PSA_ERROR_GENERIC_ERROR;
            }
        }
        psa_destroy_key(key_id);
    }
    psa_reset_key_attributes(&attr);
    if (status == PSA_SUCCESS) {
        smp_sc_reverse(priv_le, priv_be, 32);
        smp_sc_reverse(px_le, pub + 1, 32);
        smp_sc_reverse(py_le, pub + 33, 32);
    }
    memset(priv_be, 0, sizeof(priv_be));
    return status;
}

/* Compute DHKey from LE inputs; output LE. Returns 0 on success. The peer
 * point is validated by PSA (invalid / off-curve keys are refused). */
static int smp_sc_dhkey(const uint8_t priv_le[32], const uint8_t px_le[32],
                        const uint8_t py_le[32], uint8_t dhkey_le[32]) {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_key_id_t key_id = 0;
    psa_status_t status;
    uint8_t priv_be[32], peer[65], shared_be[32];
    size_t len = 0;

    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        return status;
    }
    smp_sc_reverse(priv_be, priv_le, 32);
    peer[0] = 0x04;
    smp_sc_reverse(peer + 1, px_le, 32);
    smp_sc_reverse(peer + 33, py_le, 32);

    smp_sc_ecc_attr(&attr, PSA_KEY_USAGE_DERIVE);
    status = psa_import_key(&attr, priv_be, sizeof(priv_be), &key_id);
    if (status == PSA_SUCCESS) {
        status = psa_raw_key_agreement(PSA_ALG_ECDH, key_id, peer, sizeof(peer),
            shared_be, sizeof(shared_be), &len);
        if (status == PSA_SUCCESS && len != sizeof(shared_be)) {
            status = PSA_ERROR_GENERIC_ERROR;
        }
        psa_destroy_key(key_id);
    }
    psa_reset_key_attributes(&attr);
    if (status == PSA_SUCCESS) {
        smp_sc_reverse(dhkey_le, shared_be, 32);
    }
    memset(priv_be, 0, sizeof(priv_be));
    memset(shared_be, 0, sizeof(shared_be));
    return status;
}

/* ---------------------------------------------------------------- */
/* SC pairing state machine (initiator / master role, kernel flow)   */
/* ---------------------------------------------------------------- */
/* New connection or slot reuse: forget everything, including the attempt. */
void smp_sc_reset(struct bt_dev *device) {
    if (device && device->ids.id < BT_MAX_DEV) {
        memset(&smp_sc[device->ids.id], 0, sizeof(smp_sc[0]));
        smp_sc_attempted[device->ids.id] = false;
    }
}

/* Pairing done (or abandoned): wipe secrets, keep the attempt flag. */
void smp_sc_clear(struct bt_dev *device) {
    if (device && device->ids.id < BT_MAX_DEV) {
        memset(&smp_sc[device->ids.id], 0, sizeof(smp_sc[0]));
    }
}

bool smp_sc_is_active(struct bt_dev *device) {
    return device->ids.id < BT_MAX_DEV && smp_sc[device->ids.id].active;
}

bool smp_sc_attempted_get(struct bt_dev *device) {
    return device->ids.id < BT_MAX_DEV && smp_sc_attempted[device->ids.id];
}

void smp_sc_attempted_set(struct bt_dev *device) {
    if (device->ids.id < BT_MAX_DEV) {
        smp_sc_attempted[device->ids.id] = true;
    }
}

/* Build the 7-byte A1/A2 (type + address, LE as stored). For the kernel,
 * a[6]=addr_type, a[0..5]=addr (6 bytes). Wire address is little-endian. */
static void sc_get_addr_le(uint8_t a[7], const bt_addr_le_t *addr) {
    memcpy(a, addr->a.val, 6);
    a[6] = addr->type;
}

/* Begin SC: called after Pairing Response with SC accepted. Generates our
 * key pair and sends the SMP Public Key PDU. */
int smp_sc_begin(struct bt_dev *device, struct bt_smp_pairing *pairing_rsp,
                 const bt_addr_le_t *remote, const bt_addr_le_t *local) {
    struct smp_sc_ctx *ctx = get_ctx(device);

    memset(ctx, 0, sizeof(*ctx));
    ctx->active = true;
    ctx->remote = *remote;
    ctx->local = *local;

    /* IO capabilities for f6 DHKey Check, order:
     *   io_cap[0] = auth_req, io_cap[1] = oob_flag, io_cap[2] = io_capability
     * (from the Pairing Request / Response PDU bytes). */
    ctx->iocap_a[0] = device->preq[3];   /* auth_req */
    ctx->iocap_a[1] = device->preq[2];   /* oob */
    ctx->iocap_a[2] = device->preq[1];   /* io_capability */
    ctx->iocap_b[0] = pairing_rsp->auth_req;
    ctx->iocap_b[1] = pairing_rsp->oob_flag;
    ctx->iocap_b[2] = pairing_rsp->io_capability;

    /* Generate our P-256 key pair. LE wire order. */
    if (smp_sc_keygen(ctx->priv, ctx->pub_x, ctx->pub_y) != 0) {
        printf("# SMP SC: key generation failed\n");
        memset(ctx, 0, sizeof(*ctx));
        return -1;
    }

    struct {
        uint8_t x[32];
        uint8_t y[32];
    } __packed pub;
    memcpy(pub.x, ctx->pub_x, 32);
    memcpy(pub.y, ctx->pub_y, 32);

    printf("# SMP SC: send public key\n");
    bt_smp_cmd_data(device->acl_handle, BT_SMP_CMD_PUBLIC_KEY, (uint8_t *)&pub, sizeof(pub));
    return 0;
}

/* Received SMP Public Key (x_le + y_le, raw wire bytes). Computes DHKey and
 * our confirm Ea. Returns 0 on success (fills confirm_out LE). */
int smp_sc_handle_public_key(struct bt_dev *device, const uint8_t *x_le,
                             const uint8_t *y_le, uint8_t confirm_out[16]) {
    struct smp_sc_ctx *ctx = get_ctx(device);
    int ret;

    memcpy(ctx->peer_x, x_le, 32);
    memcpy(ctx->peer_y, y_le, 32);

    ret = smp_sc_dhkey(ctx->priv, ctx->peer_x, ctx->peer_y, ctx->dhkey);
    if (ret != 0) {
        printf("# SMP SC: DHKey calculation failed %d\n", ret);
        return -1;
    }

    /* Generate our nonce Na (initiator/master), LE */
    esp_fill_random(ctx->na, 16);

    /* Ea = f4(own_X, peer_X, Na, 0) - LE */
    if (sc_f4(ctx->pub_x, ctx->peer_x, ctx->na, 0, ctx->eown) != 0) {
        return -1;
    }
    memcpy(confirm_out, ctx->eown, 16);

    printf("# SMP SC: public key received, dhkey ok\n");
    return 0;
}

const uint8_t *smp_sc_get_na(struct bt_dev *device) {
    return get_ctx(device)->na;
}

void smp_sc_store_confirm(struct bt_dev *device, const uint8_t val[16]) {
    struct smp_sc_ctx *ctx = get_ctx(device);
    memcpy(ctx->epeer, val, 16);
}

/* Received SMP Pairing Random (peer nonce Nb, LE). Verifies peer confirm
 * (kernel: f4(remote_pk, local_pk, rrnd, 0)), derives keys, computes our
 * DHKey Check. Fills dhk_out (LE). */
int smp_sc_handle_random(struct bt_dev *device, const uint8_t val[16],
                         uint8_t dhk_out[16]) {
    struct smp_sc_ctx *ctx = get_ctx(device);
    uint8_t a[7], b[7], check[16], zero[16] = {0};

    memcpy(ctx->nb, val, 16);

    /* Verify peer confirm: Cb ?= f4(peer_X, own_X, Nb, 0) (kernel initiator).
     * Some peers (e.g. the Steam Controller Triton) compute their confirm with
     * a byte ordering that differs from the kernel LE conventions
     * for this step. The authoritative MITM check is the DHKey
     * Check, so a confirm mismatch here is logged but NOT fatal: the link is
     * still secured if the DHKey Check passes. */
    if (sc_f4(ctx->peer_x, ctx->pub_x, ctx->nb, 0, check) != 0) {
        return -1;
    }
    if (memcmp(check, ctx->epeer, 16) != 0) {
        printf("# SMP SC: confirm value mismatch (non-fatal, relying on DHKey Check)\n");
        bt_mon_log(true, "# SMP SC: peer confirm mismatch (non-fatal, relying on DHKey Check)\n");
    }

    /* f5(DHKey, N1=Na, N2=Nb, A1, A2) -> MacKey | LTK (kernel) */
    sc_get_addr_le(a, &ctx->local);   /* initiator addr */
    sc_get_addr_le(b, &ctx->remote);  /* responder addr */

    uint8_t f5out[32];
    if (sc_f5(ctx->dhkey, ctx->na, ctx->nb, a, b, f5out) != 0) {
        return -1;
    }
    memcpy(ctx->mackey, f5out, 16);
    memcpy(ctx->ltk, f5out + 16, 16);

    /* DHKey Check (initiator): f6(MacKey, N1=Na, N2=Nb, r=0, ioCap=A, A1, A2) */
    if (sc_f6(ctx->mackey, ctx->na, ctx->nb, zero, ctx->iocap_a, a, b, dhk_out) != 0) {
        return -1;
    }

    printf("# SMP SC: keys derived\n");
    return 0;
}

/* Received SMP DHKey Check (LE). Verifies it. 0 on success. */
int smp_sc_handle_dhk_check(struct bt_dev *device, const uint8_t e[16]) {
    struct smp_sc_ctx *ctx = get_ctx(device);
    uint8_t a[7], b[7], check[16], zero[16] = {0};

    /* Verify responder DHKey Check: f6(MacKey, N1=Nb, N2=Na, 0, ioCap=B, A2, A1) */
    sc_get_addr_le(a, &ctx->local);
    sc_get_addr_le(b, &ctx->remote);

    if (sc_f6(ctx->mackey, ctx->nb, ctx->na, zero, ctx->iocap_b, b, a, check) != 0) {
        return -1;
    }
    if (memcmp(check, e, 16) != 0) {
        printf("# SMP SC: DHKey Check mismatch!\n");
        return -1;
    }

    printf("# SMP SC: DHKey Check verified\n");
    return 0;
}

void smp_sc_get_ltk(struct bt_dev *device, uint8_t ltk[16]) {
    memcpy(ltk, get_ctx(device)->ltk, 16);
}
