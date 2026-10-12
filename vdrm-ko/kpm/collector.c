/* PREL32 exports, absolute u32 CRC table. No guessed table strides. */
#include <compiler.h>
#include <kpmodule.h>
#include <ktypes.h>
#include <kallsyms.h>
#include <linux/printk.h>
#include "request.h"
#define OUT "/data/local/tmp/vdrm-port-session/crcs.tsv"
#define HASH_OUT "/data/local/tmp/vdrm-port-session/hashes.tsv"
struct ksym { int value_offset, name_offset, namespace_offset; };
static unsigned char found[sizeof(names) / sizeof(names[0])];
static long (*write_file)(void *, const void *, size_t, loff_t *);
static void *(*open_file)(const char *, int, unsigned int);
static int (*close_file)(void *, void *);
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static int record(void *file, loff_t *pos, const char *name, unsigned int value)
{
    char line[128];
    const char hex[] = "0123456789abcdef";
    int n = 0, k;
    while (name[n] && n < 100) { line[n] = name[n]; n++; }
    if (name[n]) return -1;
    line[n++] = '\t'; line[n++] = '0'; line[n++] = 'x';
    for (k = 7; k >= 0; k--) line[n++] = hex[(value >> (k * 4)) & 15];
    line[n++] = '\n';
    return write_file(file, line, n, pos) == n ? 0 : -1;
}
static int scan(const char *start, const char *stop,
                const char *crc_start, const char *crc_stop,
                void *file, loff_t *pos)
{
    unsigned long ks = kallsyms_lookup_name(start);
    unsigned long ke = kallsyms_lookup_name(stop);
    unsigned long cs = kallsyms_lookup_name(crc_start);
    unsigned long ce = kallsyms_lookup_name(crc_stop);
    unsigned long count, idx;
    if (!ks || !ke || !cs || !ce || ke < ks || ce < cs ||
        (ke - ks) % sizeof(struct ksym)) return -1;
    count = (ke - ks) / sizeof(struct ksym);
    if (ce - cs != count * sizeof(unsigned int)) {
        pr_err("vdrm_port_crc: unsupported table layout: %s\n", start);
        return -2;
    }
    for (idx = 0; idx < count; idx++) {
        struct ksym *e = (struct ksym *)ks + idx;
        const char *name = (const char *)&e->name_offset + e->name_offset;
        unsigned int j;
        for (j = 0; j < sizeof(names) / sizeof(names[0]); j++) {
            if (same(name, names[j])) {
                if (found[j] || record(file, pos, name, ((unsigned int *)cs)[idx])) return -3;
                found[j] = 1;
                break;
            }
        }
    }
    return 0;
}
static long crc_init(const char *args, const char *event, void *__user reserved)
{
    void *file;
    loff_t pos = 0;
    unsigned int i;
    int rc;
    open_file = (void *)kallsyms_lookup_name("filp_open");
    write_file = (void *)kallsyms_lookup_name("kernel_write");
    close_file = (void *)kallsyms_lookup_name("filp_close");
    if (!open_file || !write_file || !close_file) return -1;
    /* O_EXCL preserves earlier captures even when invoked without the host script. */
    file = open_file(OUT, 0xc1, 0600);
    if (!file || (unsigned long)file >= (unsigned long)-4095) return -2;
    rc = scan("__start___ksymtab", "__stop___ksymtab",
              "__start___kcrctab", "__stop___kcrctab", file, &pos);
    if (!rc) rc = scan("__start___ksymtab_gpl", "__stop___ksymtab_gpl",
                      "__start___kcrctab_gpl", "__stop___kcrctab_gpl", file, &pos);
    if (close_file(file, 0) && !rc) rc = -5;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!found[i]) { pr_err("vdrm_port_crc: missing export %s\n", names[i]); if (!rc) rc = -4; }
    }
    if (rc) return rc;
    if (COLLECT_KCFI) {
        pos = 0;
        file = open_file(HASH_OUT, 0xc1, 0600);
        if (!file || (unsigned long)file >= (unsigned long)-4095) return -6;
        for (i = 0; i < sizeof(hash_names) / sizeof(hash_names[0]); i++) {
            unsigned long address = kallsyms_lookup_name(hash_names[i]);
            if (address < 4096 || record(file, &pos, hash_names[i],
                                       *(volatile unsigned int *)(address - 4))) {
                rc = -7; break;
            }
        }
        if (close_file(file, 0) && !rc) rc = -8;
    }
    pr_info("vdrm_port_crc: rc=%d exports=%u\n", rc, (unsigned int)sizeof(found));
    return rc;
}
static long crc_exit(void *__user reserved) { return 0; }
KPM_NAME("vdrm_port_crc");
KPM_VERSION("2.0");
KPM_LICENSE("GPL v2");
KPM_AUTHOR("vdrm");
KPM_DESCRIPTION("Collect all requested target CRCs and callback type anchors");
KPM_INIT(crc_init);
KPM_EXIT(crc_exit);
