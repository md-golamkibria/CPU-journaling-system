#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FS_MAGIC 0x56534653U

#define BLOCK_SIZE        4096U
#define JOURNAL_BLOCK_IDX    1U
#define JOURNAL_BLOCKS      16U
#define INODE_BLOCKS         2U
#define DATA_BLOCKS         64U
#define INODE_BMAP_IDX     (JOURNAL_BLOCK_IDX + JOURNAL_BLOCKS)
#define DATA_BMAP_IDX      (INODE_BMAP_IDX + 1U)
#define INODE_START_IDX    (DATA_BMAP_IDX + 1U)
#define DATA_START_IDX     (INODE_START_IDX + INODE_BLOCKS)
#define TOTAL_BLOCKS       (DATA_START_IDX + DATA_BLOCKS)

#define JOURNAL_MAGIC 0x4A4F5552U

struct superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t journal_block;
    uint32_t inode_bitmap;
    uint32_t data_bitmap;
    uint32_t inode_start;
    uint32_t data_start;
    uint8_t  _pad[128 - 9 * 4];
};

struct journal_header {
    uint32_t magic;
    uint32_t committed;
    uint32_t nblocks;
    uint32_t reserved;
    uint32_t blocks[JOURNAL_BLOCKS - 1];
    uint8_t  pad[BLOCK_SIZE - (4 * 4) - ((JOURNAL_BLOCKS - 1) * 4)];
};

static int j_fd = -1;
static struct superblock j_sb;
static uint8_t j_opened = 0;

static uint32_t tx_blocks[JOURNAL_BLOCKS - 1];
static uint8_t  tx_payload[JOURNAL_BLOCKS - 1][BLOCK_SIZE];
static uint32_t tx_count = 0;
static uint32_t tx_active = 0;

static void die(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void pwrite_block(int fd, uint32_t block_index, const void *buf) {
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    ssize_t n = pwrite(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) die("pwrite");
}

static void pread_block(int fd, uint32_t block_index, void *buf) {
    off_t offset = (off_t)block_index * BLOCK_SIZE;
    ssize_t n = pread(fd, buf, BLOCK_SIZE, offset);
    if (n != (ssize_t)BLOCK_SIZE) die("pread");
}

static void write_journal_header(const struct journal_header *h) {
    pwrite_block(j_fd, j_sb.journal_block, h);
}

static void read_journal_header(struct journal_header *h) {
    pread_block(j_fd, j_sb.journal_block, h);
}

static void fsync_fd(int fd) {
    if (fsync(fd) != 0) die("fsync");
}

int journal_open(const char *image_path) {
    if (j_opened) return 0;
    j_fd = open(image_path, O_RDWR);
    if (j_fd < 0) return -1;
    uint8_t block[BLOCK_SIZE];
    struct superblock sb;
    pread_block(j_fd, 0, block);
    memcpy(&sb, block, sizeof(sb));
    if (sb.magic != FS_MAGIC || sb.block_size != BLOCK_SIZE || sb.total_blocks != TOTAL_BLOCKS) {
        close(j_fd);
        j_fd = -1;
        return -1;
    }
    j_sb = sb;
    j_opened = 1;
    return 0;
}

void journal_close(void) {
    if (j_fd >= 0) close(j_fd);
    j_fd = -1;
    j_opened = 0;
}

int journal_begin(void) {
    if (!j_opened) return -1;
    if (tx_active) return -1;
    tx_active = 1;
    tx_count = 0;
    return 0;
}

int journal_log_write(uint32_t blockno, const void *data) {
    if (!tx_active) return -1;
    if (tx_count >= JOURNAL_BLOCKS - 1) return -1;
    if (blockno >= TOTAL_BLOCKS) return -1;
    memcpy(tx_payload[tx_count], data, BLOCK_SIZE);
    tx_blocks[tx_count] = blockno;
    tx_count++;
    return 0;
}

static int install_transaction(const struct journal_header *h) {
    for (uint32_t i = 0; i < h->nblocks; ++i) {
        uint32_t home = h->blocks[i];
        uint32_t jblk = j_sb.journal_block + 1 + i;
        uint8_t buf[BLOCK_SIZE];
        pread_block(j_fd, jblk, buf);
        pwrite_block(j_fd, home, buf);
    }
    fsync_fd(j_fd);
    return 0;
}

int journal_commit(void) {
    if (!tx_active) return -1;
    if (tx_count == 0) { tx_active = 0; return 0; }
    struct journal_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = JOURNAL_MAGIC;
    hdr.committed = 0;
    hdr.nblocks = tx_count;
    for (uint32_t i = 0; i < tx_count; ++i) hdr.blocks[i] = tx_blocks[i];
    write_journal_header(&hdr);
    fsync_fd(j_fd);
    for (uint32_t i = 0; i < tx_count; ++i) {
        uint32_t jblk = j_sb.journal_block + 1 + i;
        pwrite_block(j_fd, jblk, tx_payload[i]);
    }
    fsync_fd(j_fd);
    hdr.committed = 1;
    write_journal_header(&hdr);
    fsync_fd(j_fd);
    install_transaction(&hdr);
    memset(&hdr, 0, sizeof(hdr));
    write_journal_header(&hdr);
    fsync_fd(j_fd);
    tx_active = 0;
    tx_count = 0;
    return 0;
}

int journal_recover(void) {
    if (!j_opened) return -1;
    struct journal_header hdr;
    read_journal_header(&hdr);
    if (hdr.magic != JOURNAL_MAGIC) return 0;
    if (hdr.committed && hdr.nblocks > 0 && hdr.nblocks <= (JOURNAL_BLOCKS - 1)) {
        install_transaction(&hdr);
        memset(&hdr, 0, sizeof(hdr));
        write_journal_header(&hdr);
        fsync_fd(j_fd);
    }
    return 0;
}
