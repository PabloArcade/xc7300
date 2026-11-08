#ifndef DECODE_H
#define DECODE_H
#include "jed.h"

typedef enum { ROW_DISABLED = 0, ROW_ACTIVE = 1, ROW_UNUSED = 2 } RowState;

/* helpers the decoder needs; implemented in main.c beside the code that
 * already computes these addresses. Never re-derive a fuse address;
 * always call the code that owns it. */
int      shared_pt_count(const JedFile *jf);
int      shared_pt_claimed(const JedFile *jf, int fb, int mc, int i);
RowState shared_pt_state(const JedFile *jf, int fb, int i);
int      shared_pt_slot_used(const JedFile *jf, int fb, int i, int slot, int *neg);
int      input_pad_registered(const JedFile *jf, int pin);
int      input_pad_ce(const JedFile *jf, int pin);
int      input_pad_rclk_pin(const JedFile *jf, int pin);
int      mc_in_use(const JedFile *jf, int fb, int mc);
/* how many PRIVATE product-term rows (fb,mc) has -- 5 everywhere
 * except an exporting MC1 on XC7372 (6). Was static in
 * decode.c; exposed here so main.c's several diagnostic readers share the
 * ONE definition instead of re-deriving it. */
int      mc_priv_rows_x(const JedFile *jf, int fb, int mc);
int      cfg_bit(const JedFile *jf, int fb, int mc, int blk);
int device_is_73144(const JedFile *jf);
RowState row_state(const JedFile *jf, int fb, int mc, int row);
int      device_row_per_record(const JedFile *jf);

#define FMT_UNKNOWN 0
#define FMT_JEDEC   1
#define FMT_IHEX    2
typedef struct { size_t records, bytes, bad_checksums; long lo, hi; } IhexInfo;
int      ihex_probe(const char *path, IhexInfo *info);
int      file_format(const char *path);
int      x108_pad_inverted(const JedFile *jf, int pin);
/* rank (1-12) -> real ball name, PG144/BG225 dedicated
 * pads only. See main.c. */
int      x108_pad_ball(const JedFile *jf, int rank, char *out, size_t outsz);
int      slot_used(const JedFile *jf, int fb, int mc, int row, int slot, int *neg);
int      uim_slot_decode_dev(const JedFile *jf, int slot, int *srcfb,
                             int *srcmc, int *isout);
int      pin_of_slot_7336(int slot);
int      pin_of_slot_dev(const JedFile *jf, int slot);
int      cfg_pin_uim_split(const JedFile *jf, int fb, int mc);
int      cfg_polarity_bit(const JedFile *jf, int fb, int mc);
int      cfg_attr(const JedFile *jf, int fb, int mc, int which);
int      localfb_blk(const JedFile *jf);
int      mc_drives_uim(const JedFile *jf, int fb, int mc);
int      uim_sources_for_line(const JedFile *jf, int fb, int line,
                             int *slots, int cap);
int      uim_slot_decode(int slot, int *srcfb, int *srcmc, int *isout);
int      uim_source_for_line(const JedFile *jf, int fb, int line,
                             int *slot, int *srcfb, int *srcmc, int *isout);
int      pin_for_macrocell(const char *device, const char *pkg, int fb, int mc);
int      decode_emit(const char *jedpath, const char *base);

/* The target gate -- see main.c */
int      fast_upto_of(const JedFile *jf);
extern const char *g_package;
void     split_part(const char *s, char *fam, size_t fl, char *pkg, size_t pl);
void     print_target(const JedFile *jf);
int      geometry_gate(const JedFile *jf);
int      target_gate(const JedFile *jf, int needs_pinout);
int      device_is_73144_note(const JedFile *jf);
#endif
