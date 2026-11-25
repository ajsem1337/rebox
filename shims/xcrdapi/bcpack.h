/* BCPack: the lossless packing of BC1 and BC3-BC7 block streams that the Xbox Series'
 * DirectStorage hardware unpacks (request options bits 1-4). Decoded in process by bcpack.c.
 *
 * Decoding needs constant tables (entropy models, index rank tables, BC7 control words and
 * per-format dictionaries). They are Microsoft's format data, not part of Rebox: the caller loads
 * them from a file the user supplies (README.md, "BCPack tables") and parses them here.
 *
 * Plain C99 with no platform headers, so tests/bcpack_test.c builds it natively too. */
#ifndef REBOX_BCPACK_H
#define REBOX_BCPACK_H

#include <stddef.h>
#include <stdint.h>

#define BCPACK_TABLES_SIZE 77982

/* Results are HRESULTs. */
#define BCPACK_OK 0
#define BCPACK_E_NOTIMPL ((int32_t)0x80004001)      /* no tables, or a codec BCPack doesn't have */
#define BCPACK_E_INVALID_DATA ((int32_t)0x8007000D) /* the stream is corrupt or ends early */
#define BCPACK_E_INVALIDARG ((int32_t)0x80070057)   /* destination empty or not whole blocks */
#define BCPACK_E_OUTOFMEMORY ((int32_t)0x8007000E)

struct bcpack_tables;

/* Parses and checks a table file image (77,982 bytes; its models must tile). NULL on failure, with
 * the reason in *why when why isn't NULL. The image isn't referenced afterwards. */
struct bcpack_tables *bcpack_tables_parse(const uint8_t *data, size_t size, const char **why);
void bcpack_tables_free(struct bcpack_tables *tables);

/* Bytes per block of a codec (the request's BC number: 1, 3, 4, 5, 6 BC6H, 7), 0 if unsupported. */
unsigned bcpack_block_size(unsigned codec);

/* Decodes a packed stream into dst_size bytes of standard BCn blocks. Bytes after the last one the
 * blocks need are ignored; *consumed (if not NULL) gets how many were read. Safe on any input:
 * corrupt data either fails or decodes to garbage blocks. tables NULL gives BCPACK_E_NOTIMPL. */
int32_t bcpack_decode(const struct bcpack_tables *tables, unsigned codec, const uint8_t *src, size_t src_size,
                      uint8_t *dst, size_t dst_size, size_t *consumed);

/* The entropy layer alone: each block's record (lanes x symbols per lane: 12 for BC1 and BC4, 24
 * for BC3 and BC5, 32 for BC6H and BC7) for `blocks` blocks, into records. For tests. */
int32_t bcpack_unpack_records(const struct bcpack_tables *tables, unsigned codec, const uint8_t *src,
                              size_t src_size, size_t blocks, uint16_t *records, size_t *consumed);
unsigned bcpack_record_length(unsigned codec);

#endif
