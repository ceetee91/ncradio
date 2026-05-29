#pragma once
#include <stdint.h>

#define RDS_PS_LEN  8   /* Program Service name (station name) */
#define RDS_RT_LEN  64  /* Radio Text */

typedef struct {
    /* group assembly: last received block words */
    uint16_t pending_b, pending_c, pending_d;
    int      b_ok, c_ok, d_ok;   /* 0 if block had uncorrectable error */

    /* decoded output */
    char     ps[RDS_PS_LEN + 1];
    char     rt[RDS_RT_LEN + 1];
    int      ps_ready;            /* all 4 PS segments received */
    int      rt_ready;
    uint8_t  ps_seg_seen;         /* bits 0-3: which PS segments received */
    uint16_t rt_seg_seen;         /* bits 0-15: which RT segments received */
    int      rt_ab;               /* current RT A/B flag (-1 = unknown) */
} RdsDecoder;

void rds_init(RdsDecoder *d);
/* Feed one raw V4L2 RDS block (struct v4l2_rds_data fields) */
void rds_feed(RdsDecoder *d, uint8_t lsb, uint8_t msb, uint8_t block_desc);
