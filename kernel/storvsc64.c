/* tinyos x64 Hyper-V synthetic SCSI (StorVSC) block driver — Stage 2 persistence.
 *
 * Hyper-V Gen 2 VMs reach their disks only through synthetic SCSI over VMBus, so
 * this is the path to a PERSISTENT filesystem on the daily-use Hyper-V machine.
 * It reuses the VMBus connection + primitives that kernel/vmbus64.c established
 * for the synthetic keyboard: find the SCSI channel offer, open a second channel
 * (its own GPADL ring), then run the vstor protocol handshake.
 *
 * VMBus can only be validated on real Hyper-V (never QEMU), so this is brought up
 * in staged, heavily-logged increments like the keyboard.  storvsc_stage() (1..12)
 * shows how far it got; on any failure storvsc_init() returns 0 and kernel/fat64.c
 * keeps the working RAM disk — so a half-finished driver never breaks the FS.
 *
 * INCREMENT 1: channel open + vstor handshake (pins the protocol version, stage 8).
 * INCREMENT 2 (this file): SCSI READ(10)/WRITE(10) via a GPA-direct data buffer.
 * A pattern write/read/verify self-test on a scratch sector (stages 9/10/11) must
 * pass before the disk is claimed.  The FAT16 volume lives in a dedicated GPT
 * data partition at STOR_BASE_LBA; only the self-test remains in the safe gap. */
#include "boot64.h"

extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);

/* synthetic SCSI controller interface GUID ba6163d9-04a1-4d29-b605-72e2ffb1dc7f */
static const uint8_t SCSI_GUID[16] = {
    0xd9,0x63,0x61,0xba, 0xa1,0x04, 0x29,0x4d, 0xb6,0x05, 0x72,0xe2,0xff,0xb1,0xdc,0x7f
};

/* channel messages (match kernel/vmbus64.c) */
#define MSG_GPADL_HEADER   8
#define MSG_GPADL_CREATED  10
#define MSG_OPENCHANNEL    5
#define MSG_OPEN_RESULT    6
#define VM_PKT_DATA_INBAND 6

/* vstor operations */
#define OP_COMPLETE_IO   1
#define OP_EXECUTE_SRB   3
#define OP_BEGIN_INIT    7
#define OP_END_INIT      8
#define OP_QUERY_VER     9
#define OP_QUERY_PROPS   10
#define REQUEST_COMPLETION_FLAG 1      /* vstor.flags: ask the host for a reply */

/* A request whose data buffer is handed to the host as raw guest-physical pages;
 * SCSI READ/WRITE data is DMA'd straight into/out of that buffer.  (Type 9 —
 * type 7 is USING_XFER_PAGES, a different mechanism the host would reject.) */
#define VM_PKT_DATA_GPA_DIRECT 9

/* SCSI request block (vmscsi_request) flags / data directions (Windows/Linux). */
#define SRB_FLAGS_DATA_IN                0x00000040
#define SRB_FLAGS_DATA_OUT               0x00000080
#define SRB_FLAGS_DISABLE_SYNCH_TRANSFER 0x00000008
#define SRB_STATUS_SUCCESS 0x01
#define VMSCSI_WRITE 0                 /* data_in: guest -> disk */
#define VMSCSI_READ  1                 /* data_in: disk  -> guest */
#define VMSCSI_NODATA 2                /* data_in: no transfer (e.g. TEST UNIT READY) */
#define SECTOR 512

/* The Hyper-V boot disk has a 64 MiB ESP at LBA 2048 followed by the dedicated
 * TinyOS FAT16 data partition.  Keep the scratch self-test in the pre-ESP gap. */
#define STOR_BASE_LBA    133120        /* FS sector 0 maps to GPT data partition */
#define STOR_SCRATCH_LBA 48            /* unused gap sector for the I/O self-test */

/* ring buffer: 1 control + 2 data pages per direction (same shape as vmbus64) */
#define RDP 2
#define SP  (1 + RDP)
#define RP  (1 + RDP)
#define RING_PAGES (SP + RP)
#define RING_SZ    (RDP * 4096u)
#define STOR_GPADL  0x2000
#define STOR_OPENID 2

static uint32_t g_relid, g_connid, g_io_diag = 0xFFFFFFFF, g_blksz;
static uint8_t *g_send, *g_recv, *g_dma;
static int      g_stage, g_ver, g_ready;
static uint64_t g_txid = 1;

/* SCSI request block — exactly 0x34 bytes so it fills the vstor packet body.
 * The final six fields were added in Windows 8 (protocol >= 5.1). */
struct vmscsi {
    uint16_t length;                  /* sizeof(this): 0x34 win8+, else 0x24 */
    uint8_t  srb_status;
    uint8_t  scsi_status;
    uint8_t  port_number;
    uint8_t  path_id;
    uint8_t  target_id;
    uint8_t  lun;
    uint8_t  cdb_length;
    uint8_t  sense_info_length;
    uint8_t  data_in;
    uint8_t  reserved;
    uint32_t data_transfer_length;
    uint8_t  cdb[0x14];               /* cdb / sense_data union (max 20 bytes) */
    uint16_t reserve;                 /* ---- Windows 8 additions ---- */
    uint8_t  queue_tag;
    uint8_t  queue_action;
    uint32_t srb_flags;
    uint32_t time_out_value;
    uint32_t queue_sort_ey;
} __attribute__((packed));

/* vstor packet: 12-byte header (op/flags/status, all u32) + a 0x34 body union
 * (version / properties / SRB).  Matches the Hyper-V vstorage wire layout. */
struct vstor {
    uint32_t op;
    uint32_t flags;
    uint32_t status;
    union {
        uint8_t       body[0x34];
        struct vmscsi srb;
    };
} __attribute__((packed));

/* ---- ring send / receive (this channel's own ring) ---- */
static void ring_out(uint8_t *data, uint32_t *w, const void *src, uint32_t n) {
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) { data[*w] = s[i]; *w = (*w + 1) % RING_SZ; }
}
static void stor_send(const void *payload, uint32_t plen, int want_comp) {
    volatile uint32_t *ctl = (volatile uint32_t *)g_send;
    uint8_t *data = g_send + 4096;
    uint32_t w = ctl[0], start = w;
    uint32_t pad = (uint32_t)((-(int)plen) & 7);
    uint8_t desc[16]; memset(desc, 0, 16);
    *(uint16_t *)(desc + 0) = VM_PKT_DATA_INBAND;
    *(uint16_t *)(desc + 2) = 2;                                  /* data offset (16/8) */
    *(uint16_t *)(desc + 4) = (uint16_t)((16 + plen + pad) / 8);  /* total len8         */
    *(uint16_t *)(desc + 6) = want_comp ? 1 : 0;                  /* completion request */
    *(uint64_t *)(desc + 8) = g_txid++;                           /* transaction id     */
    ring_out(data, &w, desc, 16);
    ring_out(data, &w, payload, plen);
    uint8_t z[8] = {0}; ring_out(data, &w, z, pad);
    uint64_t prev = start; ring_out(data, &w, &prev, 8);
    __asm__ volatile("mfence" ::: "memory");
    ctl[0] = w;
    __asm__ volatile("mfence" ::: "memory");
    vmbus_signal_event(g_connid);
}
static uint32_t stor_recv(void *buf, uint32_t maxlen) {
    volatile uint32_t *ctl = (volatile uint32_t *)g_recv;
    const uint8_t *data = g_recv + 4096;
    uint32_t r = ctl[1], wr = ctl[0];
    if (r == wr) return 0;
    __asm__ volatile("mfence" ::: "memory");
    uint8_t hdr[16];
    for (uint32_t i = 0; i < 16; i++) hdr[i] = data[(r + i) % RING_SZ];
    uint16_t off8 = *(uint16_t *)(hdr + 2), len8 = *(uint16_t *)(hdr + 4);
    if (!len8) return 0;
    uint32_t ppos = (r + off8 * 8u) % RING_SZ;
    uint32_t plen = (len8 - off8) * 8u;
    if (plen > maxlen) plen = maxlen;
    uint8_t *d = (uint8_t *)buf;
    for (uint32_t i = 0; i < plen; i++) d[i] = data[(ppos + i) % RING_SZ];
    ctl[1] = (r + len8 * 8u + 8u) % RING_SZ;
    __asm__ volatile("mfence" ::: "memory");
    return plen;
}

/* send a vstor packet, poll for the host completion; 0 = got a reply */
static int vstor_txn(struct vstor *pkt, struct vstor *reply) {
    pkt->flags = REQUEST_COMPLETION_FLAG;
    stor_send(pkt, sizeof(*pkt), 1);
    for (int t = 0; t < 400; t++) {
        uint8_t b[80];
        uint32_t n = stor_recv(b, sizeof(b));
        if (n >= 8) { memset(reply, 0, sizeof(*reply)); memcpy(reply, b, n < sizeof(*reply) ? n : sizeof(*reply)); return 0; }
        for (volatile int d = 0; d < 1000000; d++) __asm__ volatile("pause");
    }
    return -1;
}

/* ---- SCSI command: build the SRB for an arbitrary CDB + data length ---- */
static void build_srb(struct vmscsi *s, const uint8_t *cdb, int cdb_len, uint8_t dir, uint32_t xfer) {
    memset(s, 0, sizeof(*s));
    s->length               = 0x34;                          /* win8+ SRB (proto >= 5.1) */
    s->cdb_length           = (uint8_t)cdb_len;
    s->sense_info_length    = 0x14;
    s->data_in              = dir;
    s->data_transfer_length = xfer;
    s->srb_flags            = SRB_FLAGS_DISABLE_SYNCH_TRANSFER |
                              (dir == VMSCSI_READ  ? SRB_FLAGS_DATA_IN  :
                               dir == VMSCSI_WRITE ? SRB_FLAGS_DATA_OUT : 0);
    s->time_out_value       = 60;
    for (int i = 0; i < cdb_len && i < 0x10; i++) s->cdb[i] = cdb[i];
}

/* Send an EXECUTE_SRB whose data buffer is g_dma (one page), then poll the ring
 * for its COMPLETE_IO reply.  Returns 0 and fills *reply on a completion. */
static int stor_io(const struct vmscsi *srb, uint32_t xfer, struct vstor *reply) {
    struct vstor pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op    = OP_EXECUTE_SRB;
    pkt.flags = REQUEST_COMPLETION_FLAG;
    pkt.srb   = *srb;

    volatile uint32_t *ctl = (volatile uint32_t *)g_send;
    uint8_t *data = g_send + 4096;
    uint32_t w = ctl[0], start = w;

    uint64_t pa   = (uint64_t)(uintptr_t)g_dma;                 /* VA == PA (identity map) */
    uint32_t plen = sizeof(pkt);
    uint16_t off8 = (16 + 24) / 8;                              /* desc + gpa header = 40  */
    uint16_t len8 = (uint16_t)((off8 * 8 + plen) / 8);          /* 40 + 64 = 104, 8-aligned */

    uint8_t desc[16]; memset(desc, 0, 16);
    *(uint16_t *)(desc + 0) = VM_PKT_DATA_GPA_DIRECT;
    *(uint16_t *)(desc + 2) = off8;
    *(uint16_t *)(desc + 4) = len8;
    *(uint16_t *)(desc + 6) = 1;                                /* completion requested */
    *(uint64_t *)(desc + 8) = g_txid++;
    ring_out(data, &w, desc, 16);

    uint32_t ghdr[2] = { 0, 1 };                               /* reserved, rangecount = 1 */
    ring_out(data, &w, ghdr, 8);
    uint32_t rng[2]  = { xfer, (uint32_t)(pa & 0xFFF) };        /* byte_count, byte_offset  */
    ring_out(data, &w, rng, 8);
    uint64_t pfn = pa >> 12; ring_out(data, &w, &pfn, 8);

    ring_out(data, &w, &pkt, plen);
    uint64_t prev = start; ring_out(data, &w, &prev, 8);
    __asm__ volatile("mfence" ::: "memory");
    ctl[0] = w;
    __asm__ volatile("mfence" ::: "memory");
    vmbus_signal_event(g_connid);

    for (int t = 0; t < 800; t++) {
        uint8_t b[80];
        uint32_t n = stor_recv(b, sizeof(b));
        if (n >= 12) {
            memset(reply, 0, sizeof(*reply));
            memcpy(reply, b, n < sizeof(*reply) ? n : sizeof(*reply));
            if (reply->op == OP_COMPLETE_IO) {
                g_io_diag = ((uint32_t)reply->status << 16) |
                            ((uint32_t)reply->srb.srb_status << 8) | reply->srb.scsi_status;
                return 0;
            }
        } else {
            for (volatile int d = 0; d < 500000; d++) __asm__ volatile("pause");
        }
    }
    g_io_diag = 0xFFFFFFFF;                 /* no completion (timeout) */
    return -1;
}

/* One 512-byte sector at a physical LBA into/out of g_dma; 0 on SRB success. */
static int do_io(uint32_t phys_lba, uint8_t dir) {
    uint8_t cdb[10]; memset(cdb, 0, sizeof(cdb));
    cdb[0] = (dir == VMSCSI_READ) ? 0x28 : 0x2A;            /* READ(10) / WRITE(10) */
    cdb[2] = (uint8_t)(phys_lba >> 24);
    cdb[3] = (uint8_t)(phys_lba >> 16);
    cdb[4] = (uint8_t)(phys_lba >> 8);
    cdb[5] = (uint8_t)(phys_lba);
    cdb[8] = 1;                                             /* one block */
    struct vmscsi srb; build_srb(&srb, cdb, 10, dir, SECTOR);
    struct vstor rep;
    if (stor_io(&srb, SECTOR, &rep)) return -1;             /* no completion (timeout) */
    if ((rep.srb.srb_status & 0x3F) != SRB_STATUS_SUCCESS) return -1;   /* SCSI-level reject */
    return 0;
}

/* READ CAPACITY(10): 8-byte reply = [last LBA][block size], both big-endian.  It
 * rides the same SRB + GPA-direct path as data I/O, so accepting it proves the
 * request path is sound and yields the disk's real block size (our sector I/O
 * assumes 512 — any other value is what a plain READ/WRITE would trip over). */
static int read_capacity(uint32_t *last_lba, uint32_t *blksz) {
    uint8_t cdb[10]; memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;                                          /* READ CAPACITY(10) */
    struct vmscsi srb; build_srb(&srb, cdb, 10, VMSCSI_READ, 8);
    struct vstor rep;
    if (stor_io(&srb, 8, &rep)) return -1;
    if ((rep.srb.srb_status & 0x3F) != SRB_STATUS_SUCCESS) return -1;
    if (last_lba) *last_lba = ((uint32_t)g_dma[0] << 24) | ((uint32_t)g_dma[1] << 16) | ((uint32_t)g_dma[2] << 8) | g_dma[3];
    if (blksz)    *blksz    = ((uint32_t)g_dma[4] << 24) | ((uint32_t)g_dma[5] << 16) | ((uint32_t)g_dma[6] << 8) | g_dma[7];
    return 0;
}

/* ---- open the SCSI channel: hand the ring pages over, then OPENCHANNEL ---- */
static int open_channel(void) {
    uint8_t *ring = (uint8_t *)vmbus_palloc(RING_PAGES);
    if (!ring) { kputs("[stor] ring alloc failed\n"); return 0; }
    g_send = ring; g_recv = ring + SP * 4096;

    uint8_t msg[256]; memset(msg, 0, sizeof(msg));
    uint32_t *m = (uint32_t *)msg;
    m[0] = MSG_GPADL_HEADER;
    m[2] = g_relid;
    m[3] = STOR_GPADL;
    uint16_t *m16 = (uint16_t *)(msg + 16);
    m16[0] = (uint16_t)(8 + 8 * RING_PAGES);      /* range buffer length */
    m16[1] = 1;                                   /* range count         */
    uint32_t *rng = (uint32_t *)(msg + 20);
    rng[0] = RING_PAGES * 4096; rng[1] = 0;
    uint64_t base = (uint64_t)(uintptr_t)ring, *pfn = (uint64_t *)(msg + 28);
    for (int i = 0; i < RING_PAGES; i++) pfn[i] = (base >> 12) + i;
    if (!vmbus_msg_post(msg, 28 + 8 * RING_PAGES)) { kputs("[stor] GPADL post failed\n"); return 0; }

    volatile uint8_t *p = vmbus_msg_poll(20000000);
    if (!p || *(volatile uint32_t *)p != MSG_GPADL_CREATED) { kputs("[stor] no GPADL_CREATED\n"); if (p) vmbus_msg_done(); return 0; }
    uint32_t gs = *(volatile uint32_t *)(p + 16); vmbus_msg_done();
    kputs("[stor] GPADL created status "); kput_u32dec(gs); kputc('\n');
    if (gs) return 0;
    g_stage = 3;

    memset(msg, 0, sizeof(msg));
    m[0] = MSG_OPENCHANNEL;
    m[2] = g_relid;
    m[3] = STOR_OPENID;
    m[4] = STOR_GPADL;
    m[5] = 0;                                     /* target vp                    */
    m[6] = SP;                                    /* recv ring page offset in GPADL */
    if (!vmbus_msg_post(msg, 28 + 120)) { kputs("[stor] OPENCHANNEL post failed\n"); return 0; }

    p = vmbus_msg_poll(20000000);
    if (!p || *(volatile uint32_t *)p != MSG_OPEN_RESULT) { kputs("[stor] no OPEN_RESULT\n"); if (p) vmbus_msg_done(); return 0; }
    uint32_t os = *(volatile uint32_t *)(p + 16); vmbus_msg_done();
    kputs("[stor] channel open status "); kput_u32dec(os); kputc('\n');
    if (os) return 0;
    g_stage = 4;
    return 1;
}

/* ---- vstor protocol handshake (pins the negotiated version) ---- */
static int handshake(void) {
    struct vstor pkt, rep;
    memset(&pkt, 0, sizeof(pkt)); pkt.op = OP_BEGIN_INIT;
    if (vstor_txn(&pkt, &rep) || rep.status) { kputs("[stor] BEGIN_INIT failed\n"); return 0; }
    g_stage = 5;

    static const uint16_t vers[] = { 0x0602, 0x0600, 0x0502, 0x0402, 0x0200 };  /* win10..win6 */
    int ok = 0;
    for (unsigned i = 0; i < sizeof(vers) / sizeof(vers[0]); i++) {
        memset(&pkt, 0, sizeof(pkt)); pkt.op = OP_QUERY_VER;
        *(uint16_t *)(pkt.body + 0) = vers[i];    /* major_minor */
        if (!vstor_txn(&pkt, &rep) && rep.status == 0) { g_ver = vers[i]; ok = 1; break; }
    }
    if (!ok) { kputs("[stor] no protocol version accepted\n"); return 0; }
    kputs("[stor] protocol version 0x"); kput_u64hex((uint64_t)(uint32_t)g_ver); kputc('\n');
    g_stage = 6;

    memset(&pkt, 0, sizeof(pkt)); pkt.op = OP_QUERY_PROPS;
    if (vstor_txn(&pkt, &rep) || rep.status) { kputs("[stor] QUERY_PROPERTIES failed\n"); return 0; }
    g_stage = 7;

    memset(&pkt, 0, sizeof(pkt)); pkt.op = OP_END_INIT;
    if (vstor_txn(&pkt, &rep) || rep.status) { kputs("[stor] END_INIT failed\n"); return 0; }
    g_stage = 8;
    kputs("[stor] vstor handshake complete (SCSI channel ready)\n");
    return 1;
}

int storvsc_init(void) {
    if (!vmbus_ready()) return 0;                 /* not Hyper-V / VMBus not connected */
    if (!vmbus_find_offer(SCSI_GUID, &g_relid, &g_connid)) {
        kputs("[stor] no synthetic SCSI channel offered\n"); return 0;
    }
    kputs("[stor] SCSI channel relid "); kput_u32dec(g_relid);
    kputs(" connid "); kput_u32dec(g_connid); kputc('\n');
    g_stage = 2;
    if (!open_channel()) return 0;
    if (!handshake())    return 0;

    /* Increment 2: prove SCSI READ/WRITE actually move data before claiming the
     * disk.  A page-aligned bounce buffer (VA==PA) is the GPA-direct target. */
    g_dma = (uint8_t *)vmbus_palloc(1);
    if (!g_dma) { kputs("[stor] DMA buffer alloc failed\n"); return 0; }
    g_ready = 1;

    /* Self-test on a scratch sector in the GPT gap: write a known pattern, read
     * it back, verify.  A non-zero pattern catches a wrong data-buffer GPA that a
     * zero-vs-zero round-trip would miss.  Stages 9/10/11 pinpoint any failure;
     * on failure return 0 so kernel/fat64.c keeps the RAM disk (no regression). */
    /* INVALID_REQUEST on every command could be a bad SRB/device-address OR a bad
     * GPA-direct data descriptor.  TEST UNIT READY carries no data, so send it INBAND
     * (no GPA range) exactly like the working handshake: if the host accepts THIS srb
     * the SRB + address are sound and the data path is at fault (stage 9 reached); if
     * it rejects this too, the SRB/address is wrong (stays at stage 8). */
    struct vstor tpkt, trep;
    memset(&tpkt, 0, sizeof(tpkt));
    tpkt.op = OP_EXECUTE_SRB;
    uint8_t tur[6] = {0};                              /* TEST UNIT READY (0x00) */
    build_srb(&tpkt.srb, tur, 6, VMSCSI_NODATA, 0);
    if (vstor_txn(&tpkt, &trep) || trep.op != OP_COMPLETE_IO) { kputs("[stor] TUR: no completion\n"); g_ready = 0; return 0; }
    g_io_diag = ((uint32_t)trep.status << 16) | ((uint32_t)trep.srb.srb_status << 8) | trep.srb.scsi_status;
    if ((trep.srb.srb_status & 0x3F) == 0x06) { kputs("[stor] TUR rejected: SRB/address\n"); g_ready = 0; return 0; }
    g_stage = 9;                                        /* SRB + address accepted (INBAND) */

    /* Now exercise the GPA-direct data path: READ CAPACITY (8-byte reply) + block size. */
    if (read_capacity(0, &g_blksz)) { kputs("[stor] READ CAPACITY failed (GPA path)\n"); g_ready = 0; return 0; }
    g_stage = 10;
    if (g_blksz != SECTOR) { kputs("[stor] disk block size is not 512\n"); g_ready = 0; return 0; }
    g_stage = 11;
    for (int i = 0; i < SECTOR; i++) g_dma[i] = (uint8_t)(i ^ 0x5A);
    if (do_io(STOR_SCRATCH_LBA, VMSCSI_WRITE)) { kputs("[stor] scratch write failed\n"); g_ready = 0; return 0; }
    memset(g_dma, 0, SECTOR);
    if (do_io(STOR_SCRATCH_LBA, VMSCSI_READ)) { kputs("[stor] scratch verify read failed\n"); g_ready = 0; return 0; }
    for (int i = 0; i < SECTOR; i++)
        if (g_dma[i] != (uint8_t)(i ^ 0x5A)) { kputs("[stor] scratch data mismatch\n"); g_ready = 0; return 0; }
    g_stage = 12;
    kputs("[stor] SCSI I/O verified — persistent disk ready\n");
    return 1;
}

int storvsc_read(uint32_t lba, void *buf) {
    if (!g_ready) return -1;
    if (do_io(STOR_BASE_LBA + lba, VMSCSI_READ)) return -1;
    memcpy(buf, g_dma, SECTOR);
    return 0;
}
int storvsc_write(uint32_t lba, const void *buf) {
    if (!g_ready) return -1;
    memcpy(g_dma, buf, SECTOR);
    return do_io(STOR_BASE_LBA + lba, VMSCSI_WRITE);
}
int storvsc_stage(void) { return g_stage; }
int storvsc_diag(void)  { return (int)g_io_diag; }
int storvsc_ver(void)   { return g_ver; }              /* negotiated vstor protocol version */
int storvsc_blksz(void) { return (int)g_blksz; }       /* disk block size from READ CAPACITY */
