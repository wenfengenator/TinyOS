/* tinyos x64 FAT16 filesystem (kernel/fat64.c) — mounts a RAM-resident image
 * embedded in the kernel (build/fatfs.h).  Read + whole-file write; one level of
 * subdirectories.  The ring-3 syscalls readdir/readfile/writefile route here. */
#ifndef TINYOS_FAT64_H
#define TINYOS_FAT64_H

#include <stdint.h>

void blk_init(void);                 /* copy the embedded image into writable RAM */
int  fat_mount(void);                /* parse the BPB; 0 on success               */
int  fat_mounted(void);

/* list usable entry #which of a directory (cluster 0 = root). out params optional */
int  fat_list(uint32_t dir_cluster, int which, char *name,
              uint8_t *attr, uint16_t *cluster, uint32_t *size);
/* case-insensitive lookup of one component inside a directory */
int  fat_lookup(uint32_t dir_cluster, const char *want,
                uint8_t *attr, uint16_t *cluster, uint32_t *size);
/* resolve a full '/'-separated path from the root ("" or "/" => root dir) */
int  fat_resolve(const char *path, uint8_t *attr, uint16_t *cluster, uint32_t *size);
/* read a file's bytes (given first cluster + size) into buf, up to maxlen */
int  fat_read(uint16_t cluster, uint32_t size, void *buf, uint32_t maxlen);
/* create/overwrite a file with len bytes; persists to the RAM image this session */
int  fat_write(const char *sub, const void *buf, uint32_t len);
/* directory create + file delete (absolute paths) */
int  fat_mkdir(const char *sub);          /* 0 ok, -1 fail                       */
int  fat_unlink(const char *sub);         /* 0 ok, -1 not found, -2 is-a-dir     */

/* current-directory helpers (a single kernel cwd all FS syscalls resolve against) */
void fs_resolve(const char *path, char *out);   /* -> absolute path (out >= 80B) */
int  fs_chdir(const char *path);                /* 0 ok, -1 not a directory      */
void fs_getcwd(char *out);                      /* copy cwd into out (>= 80B)    */

#endif /* TINYOS_FAT64_H */
