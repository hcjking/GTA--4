// GTA IV RSC5 resources (.xtd texture dictionaries) without Microsoft's
// xcompress: unpacks with libmspack's LZX decoder (what the port's runtime
// decodes with, gta4_lzx_hooks.cpp) and packs with LZX *uncompressed* blocks,
// which any LZX decoder accepts. Layout as xtd_cli.c's: the 16-byte RSC5
// header, a big-endian compressed size, then XMem frames - each a 2-byte
// big-endian compressed size for a full 32 KiB frame, or 0xFF, uncompressed
// size, compressed size (2 bytes each) for a shorter one.
//
// Usage: xtd_store unpack <in.xtd> <out.bin>
//        xtd_store pack <template.xtd> <in.bin> <out.xtd>
// Build: cc -O2 -I<libmspack>/mspack xtd_store.c <libmspack>/mspack/lzxd.c -o xtd_store
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mspack.h"
#include "lzx.h"

enum { kFrame = 0x8000, kWindowBits = 17 };

// --- a memory-backed mspack_system --------------------------------------
struct mem_file { uint8_t* data; size_t size, pos; };
static struct mspack_file* m_open(struct mspack_system* s, const char* f, int m) { (void)s; (void)m; return (struct mspack_file*)f; }
static void m_close(struct mspack_file* f) { (void)f; }
static int m_read(struct mspack_file* f, void* buf, int n) {
  struct mem_file* m = (struct mem_file*)f;
  size_t left = m->size - m->pos;
  if ((size_t)n > left) n = (int)left;
  memcpy(buf, m->data + m->pos, n); m->pos += n; return n;
}
static int m_write(struct mspack_file* f, void* buf, int n) {
  struct mem_file* m = (struct mem_file*)f;
  if (m->pos + n > m->size) return -1;
  memcpy(m->data + m->pos, buf, n); m->pos += n; return n;
}
static int m_seek(struct mspack_file* f, off_t o, int mode) { (void)f; (void)o; (void)mode; return -1; }
static off_t m_tell(struct mspack_file* f) { return (off_t)((struct mem_file*)f)->pos; }
static void m_msg(struct mspack_file* f, const char* fmt, ...) { (void)f; (void)fmt; }
static void* m_alloc(struct mspack_system* s, size_t n) { (void)s; return malloc(n); }
static void m_free(void* p) { free(p); }
static void m_copy(void* a, void* b, size_t n) { memmove(b, a, n); }
static struct mspack_system g_sys = {m_open, m_close, m_read, m_write, m_seek, m_tell, m_msg, m_alloc, m_free, m_copy, NULL};

static uint8_t* read_file(const char* path, size_t* size) {
  FILE* f = fopen(path, "rb");
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
  fseek(f, 0, SEEK_END); *size = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* data = malloc(*size ? *size : 1);
  if (fread(data, 1, *size, f) != *size) { fclose(f); free(data); return NULL; }
  fclose(f);
  return data;
}

static int write_file(const char* path, const void* data, size_t size) {
  FILE* f = fopen(path, "wb");
  if (!f || fwrite(data, 1, size, f) != size) { if (f) fclose(f); fprintf(stderr, "cannot write %s\n", path); return 1; }
  fclose(f);
  return 0;
}

// The XMem frames' LZX data joined, and the uncompressed total.
static uint8_t* join_frames(const uint8_t* p, const uint8_t* end, size_t* joined, size_t* total) {
  uint8_t* out = malloc((size_t)(end - p) + 16);
  *joined = 0; *total = 0;
  while (p < end) {
    uint32_t raw = kFrame, packed;
    if (*p == 0xFF) {
      if (p + 5 > end) break;
      raw = (uint32_t)p[1] << 8 | p[2]; packed = (uint32_t)p[3] << 8 | p[4]; p += 5;
    } else {
      if (p + 2 > end) break;
      packed = (uint32_t)p[0] << 8 | p[1]; p += 2;
    }
    if (!packed || p + packed > end) break;
    memcpy(out + *joined, p, packed); *joined += packed; *total += raw; p += packed;
  }
  return out;
}

static int unpack(const char* in, const char* out) {
  size_t size;
  uint8_t* data = read_file(in, &size);
  if (!data || size < 20 || memcmp(data, "RSC\x05", 4)) { fprintf(stderr, "not an RSC5 resource\n"); return 1; }
  size_t joined, total;
  uint8_t* lzx = join_frames(data + 20, data + size, &joined, &total);
  struct mem_file src = {lzx, joined, 0};
  struct mem_file dst = {malloc(total), total, 0};
  struct lzxd_stream* s = lzxd_init(&g_sys, (struct mspack_file*)&src, (struct mspack_file*)&dst, kWindowBits, 0, kFrame,
                                    (off_t)total, 0);
  int error = s ? lzxd_decompress(s, (off_t)total) : -1;
  if (s) lzxd_free(s);
  if (error) { fprintf(stderr, "LZX decode failed (%d)\n", error); return 1; }
  printf("unpacked %zu bytes\n", total);
  return write_file(out, dst.data, total);
}

// Bits into 16-bit little-endian words, most significant bit first.
struct bits { uint8_t* out; size_t at; uint32_t word; int used; };
static void put_bits(struct bits* b, uint32_t value, int count) {
  while (count--) {
    b->word = b->word << 1 | ((value >> count) & 1);
    if (++b->used == 16) { b->out[b->at++] = (uint8_t)b->word; b->out[b->at++] = (uint8_t)(b->word >> 8); b->word = 0; b->used = 0; }
  }
}
static void flush_word(struct bits* b) { if (b->used) put_bits(b, 0, 16 - b->used); }

static int pack(const char* template_path, const char* in, const char* out) {
  size_t header_size, size;
  uint8_t* header = read_file(template_path, &header_size);
  uint8_t* data = read_file(in, &size);
  if (!header || header_size < 16 || memcmp(header, "RSC\x05", 4) || !data) { fprintf(stderr, "bad inputs\n"); return 1; }
  // One uncompressed LZX block per 32 KiB frame: (first frame: the E8
  // translation bit, off) type 3, 24-bit length, padding to the next 16-bit
  // word, R0-R2, the bytes, and a pad byte after an odd length.
  uint8_t* file = malloc(20 + size + (size / kFrame + 1) * 40);
  memcpy(file, header, 16);
  size_t at = 20;
  for (size_t offset = 0; offset < size; offset += kFrame) {
    const uint32_t raw = (uint32_t)(size - offset < kFrame ? size - offset : kFrame);
    uint8_t frame[kFrame + 64];
    struct bits b = {frame, 0, 0, 0};
    if (offset == 0) put_bits(&b, 0, 1);
    put_bits(&b, 3, 3);
    put_bits(&b, raw, 24);
    flush_word(&b);
    for (int r = 0; r < 3; r++) { const uint8_t one[4] = {1, 0, 0, 0}; memcpy(frame + b.at, one, 4); b.at += 4; }
    memcpy(frame + b.at, data + offset, raw); b.at += raw;
    if (raw & 1) frame[b.at++] = 0;
    const uint32_t packed = (uint32_t)b.at;
    if (raw == kFrame) {
      file[at++] = (uint8_t)(packed >> 8); file[at++] = (uint8_t)packed;
    } else {
      file[at++] = 0xFF; file[at++] = (uint8_t)(raw >> 8); file[at++] = (uint8_t)raw;
      file[at++] = (uint8_t)(packed >> 8); file[at++] = (uint8_t)packed;
    }
    memcpy(file + at, frame, packed); at += packed;
  }
  const uint32_t body = (uint32_t)(at - 20);
  file[16] = (uint8_t)(body >> 24); file[17] = (uint8_t)(body >> 16); file[18] = (uint8_t)(body >> 8); file[19] = (uint8_t)body;
  printf("packed %zu bytes into %zu\n", size, at);
  return write_file(out, file, at);
}

int main(int argc, char** argv) {
  if (argc == 4 && !strcmp(argv[1], "unpack")) return unpack(argv[2], argv[3]);
  if (argc == 5 && !strcmp(argv[1], "pack")) return pack(argv[2], argv[3], argv[4]);
  fprintf(stderr, "usage: xtd_store unpack <in.xtd> <out.bin> | pack <template.xtd> <in.bin> <out.xtd>\n");
  return 2;
}
