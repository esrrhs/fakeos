package kernel;
import fs;
import types;

typedef types.uint64_t u64;

extern void kprintf(const char *fmt, ...);

/* Rootfs blob symbols emitted by user/blob.asm. */
extern char user_init_start;
extern char user_init_end;
extern char user_sh_start;
extern char user_sh_end;
extern char user_hello_start;
extern char user_hello_end;
extern char user_cat_start;
extern char user_cat_end;
extern char user_ls_start;
extern char user_ls_end;
extern char motd_start;
extern char motd_end;

static void publish_one(const char *path, u64 start, u64 end) {
    u64 size = end - start;
    if (start == 0 || size == 0 || !fs.fs_publish_file(path, start, size)) {
        kprintf("[FS] FAILED to publish %s\n", path);
        return;
    }
    kprintf("[FS] published %s (%u bytes)\n", path, size);
}

/* Release every embedded user image into the Ramfs root, creating the
 * intermediate /bin and /etc directories. Runs once after fs_init(). */
void rootfs_publish(void) {
    publish_one("/bin/init",  (u64)(&user_init_start),  (u64)(&user_init_end));
    publish_one("/bin/sh",    (u64)(&user_sh_start),    (u64)(&user_sh_end));
    publish_one("/bin/hello", (u64)(&user_hello_start), (u64)(&user_hello_end));
    publish_one("/bin/cat",   (u64)(&user_cat_start),   (u64)(&user_cat_end));
    publish_one("/bin/ls",    (u64)(&user_ls_start),    (u64)(&user_ls_end));
    publish_one("/etc/motd",  (u64)(&motd_start),       (u64)(&motd_end));
}
