/* vim: set tabstop=8 shiftwidth=4 softtabstop=4 expandtab smarttab colorcolumn=80: */
/*
 * Copyright (c) 2015 Red Hat, Inc.
 * Author: Nathaniel McCallum <npmccallum@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "clt/adv.h"
#include "clt/msg.h"
#include "luks/asn1.h"
#include "luks/meta.h"

#include <libcryptsetup.h>

#include <argp.h>
#include <stdbool.h>
#include <string.h>
#include <sysexits.h>

struct options {
    const char *device;
    const char *host;
    const char *svc;
    const char *file;
    bool listen;
};

static TANG_MSG_ADV_REP *
get_adv(const struct options *opts)
{
    TANG_MSG_ADV_REP *rep = NULL;
    TANG_MSG *msg = NULL;

    if (opts->file) {
        msg = msg_read(opts->file);
    } else {
        TANG_MSG req = { .type = TANG_MSG_TYPE_ADV_REQ };
        const TANG_MSG *reqs[] = { &req, NULL };
        STACK_OF(TANG_MSG) *reps = NULL;

        req.val.adv.req = adv_req(NULL);
        if (!req.val.adv.req)
            return NULL;

        if (opts->listen)
            reps = msg_wait(reqs, opts->host, opts->svc, 10);
        else
            reps = msg_rqst(reqs, opts->host, opts->svc, 10);

        TANG_MSG_ADV_REQ_free(req.val.adv.req);

        if (reps && SKM_sk_num(TANG_MSG, reps) == 1)
            msg = SKM_sk_pop(TANG_MSG, reps);

        SKM_sk_pop_free(TANG_MSG, reps, TANG_MSG_free);
    }

    if (msg && msg->type == TANG_MSG_TYPE_ADV_REP) {
        rep = msg->val.adv.rep;
        msg->val.adv.rep = NULL;
    }

    TANG_MSG_free(msg);
    return rep;
}

static struct crypt_device *
open_device(const char *device)
{
    struct crypt_device *cd = NULL;
    const char *type = NULL;
    int nerr = 0;

    nerr = crypt_init(&cd, device);
    if (nerr != 0) {
        fprintf(stderr, "Unable to open device (%s): %s\n",
                device, strerror(-nerr));
        return NULL;
    }

    nerr = crypt_load(cd, NULL, NULL);
    if (nerr != 0) {
        fprintf(stderr, "Unable to load device (%s): %s\n",
                device, strerror(-nerr));
        goto error;
    }

    type = crypt_get_type(cd);
    if (type == NULL) {
        fprintf(stderr, "Unable to determine device type for %s\n", device);
        goto error;
    }

    if (strcmp(type, CRYPT_LUKS1) != 0) {
        fprintf(stderr, "%s (%s) is not a LUKS device\n", device, type);
        goto error;
    }

    return cd;

error:
    crypt_free(cd);
    return NULL;
}

/* Steals rec */
static bool
store(const struct options *opts, TANG_MSG_REC_REQ *rec, int slot)
{
    TANG_LUKS *tl = NULL;
    bool status = false;
    uint8_t *buf = NULL;
    int len = 0;

    tl = TANG_LUKS_new();
    if (!tl) {
        TANG_MSG_REC_REQ_free(rec);
        goto egress;
    }

    TANG_MSG_REC_REQ_free(tl->rec);
    tl->listen = opts->listen;
    tl->rec = rec;

    if (ASN1_STRING_set(tl->host, opts->host, strlen(opts->host)) <= 0)
        goto egress;

    if (ASN1_STRING_set(tl->service, opts->svc, strlen(opts->svc)) <= 0)
        goto egress;

    len = i2d_TANG_LUKS(tl, &buf);
    if (len <= 0)
        goto egress;

    status = meta_write(opts->device, slot, buf, len);

egress:
    TANG_LUKS_free(tl);
    OPENSSL_free(buf);
    return status;
}

static int
add(const struct options *opts)
{
    struct crypt_device *cd = NULL;
    TANG_MSG_ADV_REP *adv = NULL;
    TANG_MSG_REC_REQ *rec = NULL;
    int status = EX_IOERR;
    BN_CTX *ctx = NULL;
    skey_t *key = NULL;
    skey_t *hex = NULL;
    int keysize = 0;
    int slot = 0;

    ctx = BN_CTX_new();
    if (!ctx)
        goto egress;

    cd = open_device(opts->device);
    if (!cd)
        goto egress;

    keysize = crypt_get_volume_key_size(cd);
    if (keysize < 16) { /* Less than 128-bits. */
        fprintf(stderr, "Key size (%d) is too small", keysize);
        status = EX_CONFIG;
        goto egress;
    }

    adv = get_adv(opts);
    if (!adv)
        goto egress; 

    rec = adv_rep(adv, keysize, &key, ctx);
    TANG_MSG_ADV_REP_free(adv);
    if (!rec)
        goto egress;

    hex = skey_new(key->size * 2 + 1);
    for (size_t i = 0; i < key->size; i++)
        snprintf((char *) &hex->data[i * 2], 2, "%02X", key->data[i]);

    slot = crypt_keyslot_add_by_passphrase(cd, CRYPT_ANY_SLOT, NULL,
                                           0, (char *) hex->data,
                                           hex->size - 1);
    OPENSSL_free(key);
    if (slot < 0) {
        TANG_MSG_REC_REQ_free(rec);
        goto egress;
    }

    if (!store(opts, rec, slot)) {
        crypt_keyslot_destroy(cd, slot);
        goto egress;
    }

    status = 0;

egress:
    BN_CTX_free(ctx);
    skey_free(key);
    skey_free(hex);
    crypt_free(cd);
    return status;
}

#define _STR(x) # x
#define STR(x) _STR(x)
#define SUMMARY 192

static error_t
parser(int key, char* arg, struct argp_state* state)
{
    struct options *opts = state->input;

    switch (key) {
    case SUMMARY:
        fprintf(stderr, "Add a tang key to a LUKS device");
        return EINVAL;

    case 'a':
        opts->file = arg;
        return 0;

    case 'l':
        opts->listen = true;
        return 0;

    case ARGP_KEY_END:
        if (!opts->device) {
            fprintf(stderr, "Device MUST be specified!\n");
            argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
            return EINVAL;
        }
            
        if (!opts->host && !opts->listen) {
            fprintf(stderr, "Host MUST be specified when not listening!\n");
            argp_state_help(state, stderr, ARGP_HELP_STD_HELP);
            return EINVAL;
        }

        if (!opts->svc)
            opts->svc = STR(TANG_PORT);

        return 0;

    case ARGP_KEY_ARG:
        if (!opts->device)
            opts->device = arg;
        else if (!opts->host)
            opts->host = arg;
        else if (!opts->svc)
            opts->svc = arg;
        else
            return ARGP_ERR_UNKNOWN;

        return 0;

    default:
        return ARGP_ERR_UNKNOWN;
    }
}

const char *argp_program_version = VERSION;

int
main(int argc, char *argv[])
{
    struct options options = {};
    const struct argp argp = {
        .options = (const struct argp_option[]) {
            { "adv",    'a', "file", .doc = "Advertisement file" },
            { "listen", 'l', .doc = "Listen for an incoming connection" },
            { "summary", SUMMARY, .flags = OPTION_HIDDEN },
            {}
        },
        .parser = parser,
        .args_doc = "DEVICE [HOST [PORT]]"
    };

    if (argp_parse(&argp, argc, argv, 0, NULL, &options) != 0)
        return EX_OSERR;

    return add(&options);
}

