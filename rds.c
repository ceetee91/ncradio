#include "rds.h"
#include <string.h>

/* V4L2 block descriptor bits */
#define BLOCK_TYPE_MASK  0x07
#define BLOCK_ERROR_MASK 0x80  /* uncorrectable — discard */

#define BLOCK_A      0
#define BLOCK_B      1
#define BLOCK_C      2
#define BLOCK_D      3
#define BLOCK_C_ALT  4

void rds_init(RdsDecoder *d)
{
    memset(d, 0, sizeof(*d));
    d->rt_ab = -1;
}

static void clean_str(char *s, int maxlen)
{
    for (int i = 0; i < maxlen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0x0D || c == 0x0A) { s[i] = '\0'; break; }
        if (c != '\0' && c < 0x20)  s[i] = ' ';
    }
    /* rtrim spaces */
    for (int i = maxlen - 1; i >= 0 && (s[i] == ' ' || s[i] == '\0'); i--)
        s[i] = '\0';
}

static void process_group(RdsDecoder *d)
{
    int group = (d->pending_b >> 12) & 0x0F;
    int ver   = (d->pending_b >> 11) & 0x01;  /* 0=A, 1=B */

    if (group == 0) {
        /* Group 0A/0B — PS name, 2 chars per group in block D */
        int seg = d->pending_b & 0x03;         /* segment 0-3 */
        if (d->b_ok && d->d_ok) {
            d->ps[seg * 2]     = (char)((d->pending_d >> 8) & 0xFF);
            d->ps[seg * 2 + 1] = (char)(d->pending_d & 0xFF);
            d->ps_seg_seen |= (uint8_t)(1u << seg);
            if (d->ps_seg_seen == 0x0F) {
                d->ps[RDS_PS_LEN] = '\0';
                clean_str(d->ps, RDS_PS_LEN);
                d->ps_ready = 1;
            }
        }
    } else if (group == 2 && ver == 0) {
        /* Group 2A — Radio Text, 4 chars per group (2 from C, 2 from D) */
        int ab  = (d->pending_b >> 4) & 0x01;
        int seg =  d->pending_b & 0x0F;        /* segment 0-15 */

        if (d->rt_ab >= 0 && ab != d->rt_ab) {
            /* A/B flag toggled: broadcaster started a new RT message */
            memset(d->rt, 0, sizeof(d->rt));
            d->rt_seg_seen = 0;
            d->rt_ready    = 0;
        }
        d->rt_ab = ab;

        if (d->c_ok) {
            d->rt[seg * 4 + 0] = (char)((d->pending_c >> 8) & 0xFF);
            d->rt[seg * 4 + 1] = (char)(d->pending_c & 0xFF);
        }
        if (d->d_ok) {
            d->rt[seg * 4 + 2] = (char)((d->pending_d >> 8) & 0xFF);
            d->rt[seg * 4 + 3] = (char)(d->pending_d & 0xFF);
        }
        if (d->c_ok || d->d_ok)
            d->rt_seg_seen |= (uint16_t)(1u << seg);

        /* Done when we have all segments from 0 up to and including this one */
        uint16_t needed = (uint16_t)((1u << (seg + 1)) - 1);
        if (!d->rt_ready && (d->rt_seg_seen & needed) == needed) {
            d->rt[RDS_RT_LEN] = '\0';
            clean_str(d->rt, RDS_RT_LEN);
            d->rt_ready = 1;
        }
        /* Also honour end-of-text marker (0x0D) anywhere in the buffer */
        if (!d->rt_ready) {
            for (int i = 0; i < RDS_RT_LEN; i++) {
                if (d->rt[i] == 0x0D) {
                    d->rt[i] = '\0';
                    clean_str(d->rt, i);
                    d->rt_ready = 1;
                    break;
                }
            }
        }
    }
}

void rds_feed(RdsDecoder *d, uint8_t lsb, uint8_t msb, uint8_t block_desc)
{
    if (block_desc & BLOCK_ERROR_MASK) return;  /* uncorrectable, discard */

    int      type = block_desc & BLOCK_TYPE_MASK;
    uint16_t word = ((uint16_t)msb << 8) | lsb;

    switch (type) {
    case BLOCK_B:
        d->pending_b = word;
        d->b_ok = 1;
        /* reset C/D readiness for this new group */
        d->c_ok = d->d_ok = 0;
        break;
    case BLOCK_C:
    case BLOCK_C_ALT:
        d->pending_c = word;
        d->c_ok = 1;
        break;
    case BLOCK_D:
        d->pending_d = word;
        d->d_ok = 1;
        process_group(d);
        break;
    default:
        break;  /* block A (PI code) — not used here */
    }
}
