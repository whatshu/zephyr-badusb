// SPDX-License-Identifier: Apache-2.0

#include "nologo_config.h"

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/printk.h>

#include <ff.h>

#ifndef NOLOGO_CONFIG_PATH
#define NOLOGO_CONFIG_PATH "/NAND:/config"
#endif

#define NOLOGO_CONFIG_MAX_LEN 4096

static uint8_t config_buf[NOLOGO_CONFIG_MAX_LEN + 1];
static size_t config_len;

/* FATFS mount for NAND disk (read-only) */
static FATFS nand_fatfs;
static struct fs_mount_t nand_mnt = {
    .type = FS_FATFS,
    .mnt_point = "/NAND:",
    .fs_data = &nand_fatfs,
    .storage_dev = (void *)"NAND",
    .flags = FS_MOUNT_FLAG_NO_FORMAT | FS_MOUNT_FLAG_READ_ONLY |
         FS_MOUNT_FLAG_USE_DISK_ACCESS,
};

static int nand_fatfs_mount(void)
{
    int rc;

    rc = disk_access_init("NAND");
    if (rc != 0) {
        printk("fs: disk_access_init(NAND) failed (%d)\n", rc);
        return rc;
    }

    rc = fs_mount(&nand_mnt);
    if (rc != 0) {
        printk("fs: mount %s failed (%d)\n", nand_mnt.mnt_point, rc);
    }

    return rc;
}

static void nand_fatfs_unmount(void)
{
    int rc = fs_unmount(&nand_mnt);
    if (rc != 0) {
        printk("fs: unmount failed (%d)\n", rc);
    }
}

int nologo_config_init(void)
{
    int rc;
    struct fs_file_t f;

    config_len = 0;
    config_buf[0] = 0;

    rc = nand_fatfs_mount();
    if (rc != 0) {
        return rc;
    }

    fs_file_t_init(&f);
    rc = fs_open(&f, NOLOGO_CONFIG_PATH, FS_O_READ);
    if (rc != 0) {
        printk("fs: open %s failed (%d)\n", NOLOGO_CONFIG_PATH, rc);
        nand_fatfs_unmount();
        return rc;
    }

    while (config_len < NOLOGO_CONFIG_MAX_LEN) {
        ssize_t n = fs_read(&f, &config_buf[config_len],
                    NOLOGO_CONFIG_MAX_LEN - config_len);
        if (n < 0) {
            printk("fs: read %s failed (%d)\n", NOLOGO_CONFIG_PATH, (int)n);
            (void)fs_close(&f);
            nand_fatfs_unmount();
            return (int)n;
        }
        if (n == 0) {
            break;
        }
        config_len += (size_t)n;
    }

    (void)fs_close(&f);
    nand_fatfs_unmount();

    config_buf[config_len] = 0;
    printk("fs: loaded %s (%u bytes)\n", NOLOGO_CONFIG_PATH, (unsigned)config_len);
    return 0;
}

const uint8_t *nologo_config_get(size_t *len)
{
    if (len) {
        *len = config_len;
    }
    return config_buf;
}


