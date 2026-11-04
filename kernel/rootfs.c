package kernel;
import fs;
import types;

typedef types.uint32_t u32;
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
extern char user_cp_start;
extern char user_cp_end;
extern char user_rm_start;
extern char user_rm_end;
extern char user_touch_start;
extern char user_touch_end;
extern char user_mv_start;
extern char user_mv_end;
extern char user_ping_start;
extern char user_ping_end;
extern char user_udptest_start;
extern char user_udptest_end;
extern char user_nc_start;
extern char user_nc_end;
extern char user_ed_start;
extern char user_ed_end;
extern char user_login_start;
extern char user_login_end;
extern char user_head_start;
extern char user_head_end;
extern char user_wc_start;
extern char user_wc_end;
extern char user_getent_start;
extern char user_getent_end;
extern char user_tail_start;
extern char user_tail_end;
extern char user_grep_start;
extern char user_grep_end;
extern char user_printf_start;
extern char user_printf_end;
extern char user_tee_start;
extern char user_tee_end;
extern char user_basename_start;
extern char user_basename_end;
extern char user_dirname_start;
extern char user_dirname_end;
extern char user_true_start;
extern char user_true_end;
extern char user_false_start;
extern char user_false_end;
extern char user_seq_start;
extern char user_seq_end;
extern char user_cut_start;
extern char user_cut_end;
extern char user_tr_start;
extern char user_tr_end;
extern char user_test_start;
extern char user_test_end;
extern char user_uniq_start;
extern char user_uniq_end;
extern char user_sort_start;
extern char user_sort_end;
extern char user_yes_start;
extern char user_yes_end;
extern char user_od_start;
extern char user_od_end;
extern char user_xargs_start;
extern char user_xargs_end;
extern char user_cmp_start;
extern char user_cmp_end;
extern char user_dd_start;
extern char user_dd_end;
extern char user_expr_start;
extern char user_expr_end;
extern char user_hostname_start;
extern char user_hostname_end;
extern char user_echo_start;
extern char user_echo_end;
extern char user_sleep_start;
extern char user_sleep_end;
extern char user_truncate_start;
extern char user_truncate_end;
extern char user_mkdir_start;
extern char user_mkdir_end;
extern char user_pwd_start;
extern char user_pwd_end;
extern char user_ln_start;
extern char user_ln_end;
extern char user_chmod_start;
extern char user_chmod_end;
extern char user_uname_start;
extern char user_uname_end;
extern char user_env_start;
extern char user_env_end;
extern char user_sync_start;
extern char user_sync_end;
extern char user_readlink_start;
extern char user_readlink_end;
extern char user_realpath_start;
extern char user_realpath_end;
extern char user_date_start;
extern char user_date_end;
extern char user_stat_start;
extern char user_stat_end;
extern char user_nl_start;
extern char user_nl_end;
extern char user_fold_start;
extern char user_fold_end;
extern char user_mktemp_start;
extern char user_mktemp_end;
extern char user_rev_start;
extern char user_rev_end;
extern char user_paste_start;
extern char user_paste_end;
extern char user_expand_start;
extern char user_expand_end;
extern char user_unexpand_start;
extern char user_unexpand_end;
extern char user_comm_start;
extern char user_comm_end;
extern char user_join_start;
extern char user_join_end;
extern char user_split_start;
extern char user_split_end;
extern char user_csplit_start;
extern char user_csplit_end;
extern char user_fmt_start;
extern char user_fmt_end;
extern char user_which_start;
extern char user_which_end;
extern char user_cksum_start;
extern char user_cksum_end;
extern char user_sum_start;
extern char user_sum_end;
extern char user_strings_start;
extern char user_strings_end;
extern char user_pr_start;
extern char user_pr_end;
extern char user_column_start;
extern char user_column_end;
extern char user_md5sum_start;
extern char user_md5sum_end;
extern char user_sha1sum_start;
extern char user_sha1sum_end;
extern char user_sha256sum_start;
extern char user_sha256sum_end;
extern char user_base64_start;
extern char user_base64_end;
extern char user_factor_start;
extern char user_factor_end;
extern char user_clear_start;
extern char user_clear_end;
extern char user_tty_start;
extern char user_tty_end;
extern char user_whoami_start;
extern char user_whoami_end;
extern char user_id_start;
extern char user_id_end;
extern char user_groups_start;
extern char user_groups_end;
extern char user_cal_start;
extern char user_cal_end;
extern char user_install_start;
extern char user_install_end;
extern char user_chown_start;
extern char user_chown_end;
extern char user_chgrp_start;
extern char user_chgrp_end;
extern char user_tac_start;
extern char user_tac_end;
extern char user_nproc_start;
extern char user_nproc_end;
extern char user_timeout_start;
extern char user_timeout_end;
extern char user_rmdir_start;
extern char user_rmdir_end;
extern char user_unlink_start;
extern char user_unlink_end;
extern char user_tsort_start;
extern char user_tsort_end;
extern char user_pathchk_start;
extern char user_pathchk_end;
extern char user_logname_start;
extern char user_logname_end;
extern char user_nohup_start;
extern char user_nohup_end;
extern char user_find_start;
extern char user_find_end;
extern char user_du_start;
extern char user_du_end;
extern char user_shuf_start;
extern char user_shuf_end;
extern char user_kill_start;
extern char user_kill_end;
extern char user_diff_start;
extern char user_diff_end;
extern char user_kbuild_start;
extern char user_kbuild_end;
extern char user_ldfake_start;
extern char user_ldfake_end;
extern char user_dumpbin_start;
extern char user_dumpbin_end;
extern char motd_start;
extern char motd_end;
/* Kept so user/environ.txt stays in the image (default FAKECC_PKG text). */
extern char environ_start;
extern char environ_end;
static volatile u64 environ_blob_anchor;
extern char resolv_conf_start;
extern char resolv_conf_end;
extern char hosts_file_start;
extern char hosts_file_end;
extern char passwd_file_start;
extern char passwd_file_end;
extern char group_file_start;
extern char group_file_end;

/* Generated by tools/m5_stage.sh -> build/m5blob.asm: one entry per staged
 * bootstrap file (native compiler, sources, reference objects, asm blobs). */
struct m5_blob_entry {
    const char *path;
    const char *start;
    const char *end;
};
extern struct m5_blob_entry m5_blob_table[];
extern u64 m5_blob_count;


/* Boot-static /proc/cpuinfo for /bin/nproc. Filled once from smp_cpu_count(). */
extern u32 smp_cpu_count(void);
static char cpuinfo_blob[512];

static void put_u32_dec(char *dst, u64 *at, u64 cap, u32 v) {
    char tmp[16];
    u32 n = 0;
    u32 i;
    if (v == 0) {
        tmp[0] = '0';
        n = 1;
    } else {
        while (v != 0 && n < 15) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
        for (i = 0; i < n / 2; i++) {
            char c = tmp[i];
            tmp[i] = tmp[n - 1 - i];
            tmp[n - 1 - i] = c;
        }
    }
    for (i = 0; i < n && *at < cap; i++) {
        dst[(*at)++] = tmp[i];
    }
}

static void publish_cpuinfo(void) {
    u32 ncpu = smp_cpu_count();
    u32 i;
    u64 at = 0;
    u64 cap = sizeof(cpuinfo_blob);
    const char *prefix = "processor\t: ";
    u32 pi;
    if (ncpu == 0) {
        ncpu = 1;
    }
    if (ncpu > 8) {
        ncpu = 8;
    }
    for (i = 0; i < ncpu; i++) {
        for (pi = 0; prefix[pi] != 0 && at < cap; pi++) {
            cpuinfo_blob[at++] = prefix[pi];
        }
        put_u32_dec(cpuinfo_blob, &at, cap, i);
        if (at < cap) {
            cpuinfo_blob[at++] = '\n';
        }
    }
    if (at == 0) {
        kprintf("[FS] FAILED to publish /proc/cpuinfo\n");
        return;
    }
    if (!fs.fs_publish_file("/proc/cpuinfo", (u64)cpuinfo_blob, at)) {
        kprintf("[FS] FAILED to publish /proc/cpuinfo\n");
        return;
    }
    kprintf("[FS] published /proc/cpuinfo (%u bytes, %u cpus)\n", at, (u64)ncpu);
}

static void publish_one(const char *path, u64 start, u64 end) {
    u64 size = end - start;
    /* A zero-length blob is legitimate, not a failure: /etc/fakecc.flags is
     * empty when the staged compiler takes no extra flags (a single-backend
     * fakecc that emits x86-64 ELF unconditionally). Only a missing or
     * unaligned blob is a real error. */
    if (size == 0) {
        return;
    }
    if (start == 0 || !fs.fs_publish_file(path, start, size)) {
        kprintf("[FS] FAILED to publish %s\n", path);
        return;
    }
    kprintf("[FS] published %s (%u bytes)\n", path, size);
}

/* Release every embedded user image into the Ramfs root, creating the
 * intermediate /bin and /etc directories. Runs once after fs_init(). */
void rootfs_publish(void) {
    u64 i;
    u32 ok = 0;

    publish_one("/bin/init",  (u64)(&user_init_start),  (u64)(&user_init_end));
    publish_one("/bin/sh",    (u64)(&user_sh_start),    (u64)(&user_sh_end));
    publish_one("/bin/hello", (u64)(&user_hello_start), (u64)(&user_hello_end));
    publish_one("/bin/cat",   (u64)(&user_cat_start),   (u64)(&user_cat_end));
    publish_one("/bin/ls",    (u64)(&user_ls_start),    (u64)(&user_ls_end));
    publish_one("/bin/cp",    (u64)(&user_cp_start),    (u64)(&user_cp_end));
    publish_one("/bin/rm",    (u64)(&user_rm_start),    (u64)(&user_rm_end));
    publish_one("/bin/touch", (u64)(&user_touch_start), (u64)(&user_touch_end));
    publish_one("/bin/mv",    (u64)(&user_mv_start),    (u64)(&user_mv_end));
    publish_one("/bin/ping",  (u64)(&user_ping_start),  (u64)(&user_ping_end));
    publish_one("/bin/udptest", (u64)(&user_udptest_start), (u64)(&user_udptest_end));
    publish_one("/bin/nc", (u64)(&user_nc_start), (u64)(&user_nc_end));
    publish_one("/bin/ed", (u64)(&user_ed_start), (u64)(&user_ed_end));
    publish_one("/bin/login", (u64)(&user_login_start), (u64)(&user_login_end));
    publish_one("/bin/head", (u64)(&user_head_start), (u64)(&user_head_end));
    publish_one("/bin/wc", (u64)(&user_wc_start), (u64)(&user_wc_end));
    publish_one("/bin/getent", (u64)(&user_getent_start), (u64)(&user_getent_end));
    publish_one("/bin/tail", (u64)(&user_tail_start), (u64)(&user_tail_end));
    publish_one("/bin/grep", (u64)(&user_grep_start), (u64)(&user_grep_end));
    publish_one("/bin/printf", (u64)(&user_printf_start), (u64)(&user_printf_end));
    publish_one("/bin/tee", (u64)(&user_tee_start), (u64)(&user_tee_end));
    publish_one("/bin/basename", (u64)(&user_basename_start), (u64)(&user_basename_end));
    publish_one("/bin/dirname", (u64)(&user_dirname_start), (u64)(&user_dirname_end));
    publish_one("/bin/true", (u64)(&user_true_start), (u64)(&user_true_end));
    publish_one("/bin/false", (u64)(&user_false_start), (u64)(&user_false_end));
    publish_one("/bin/seq", (u64)(&user_seq_start), (u64)(&user_seq_end));
    publish_one("/bin/cut", (u64)(&user_cut_start), (u64)(&user_cut_end));
    publish_one("/bin/tr", (u64)(&user_tr_start), (u64)(&user_tr_end));
    publish_one("/bin/test", (u64)(&user_test_start), (u64)(&user_test_end));
    publish_one("/bin/[", (u64)(&user_test_start), (u64)(&user_test_end));
    publish_one("/bin/uniq", (u64)(&user_uniq_start), (u64)(&user_uniq_end));
    publish_one("/bin/sort", (u64)(&user_sort_start), (u64)(&user_sort_end));
    publish_one("/bin/yes", (u64)(&user_yes_start), (u64)(&user_yes_end));
    publish_one("/bin/od", (u64)(&user_od_start), (u64)(&user_od_end));
    publish_one("/bin/xargs", (u64)(&user_xargs_start), (u64)(&user_xargs_end));
    publish_one("/bin/cmp", (u64)(&user_cmp_start), (u64)(&user_cmp_end));
    publish_one("/bin/dd", (u64)(&user_dd_start), (u64)(&user_dd_end));
    publish_one("/bin/expr", (u64)(&user_expr_start), (u64)(&user_expr_end));
    publish_one("/bin/hostname", (u64)(&user_hostname_start), (u64)(&user_hostname_end));
    publish_one("/bin/echo", (u64)(&user_echo_start), (u64)(&user_echo_end));
    publish_one("/bin/sleep", (u64)(&user_sleep_start), (u64)(&user_sleep_end));
    publish_one("/bin/truncate", (u64)(&user_truncate_start), (u64)(&user_truncate_end));
    publish_one("/bin/mkdir", (u64)(&user_mkdir_start), (u64)(&user_mkdir_end));
    publish_one("/bin/pwd", (u64)(&user_pwd_start), (u64)(&user_pwd_end));
    publish_one("/bin/ln", (u64)(&user_ln_start), (u64)(&user_ln_end));
    publish_one("/bin/chmod", (u64)(&user_chmod_start), (u64)(&user_chmod_end));
    publish_one("/bin/uname", (u64)(&user_uname_start), (u64)(&user_uname_end));
    publish_one("/bin/env", (u64)(&user_env_start), (u64)(&user_env_end));
    publish_one("/bin/sync", (u64)(&user_sync_start), (u64)(&user_sync_end));
    publish_one("/bin/readlink", (u64)(&user_readlink_start), (u64)(&user_readlink_end));
    publish_one("/bin/realpath", (u64)(&user_realpath_start), (u64)(&user_realpath_end));
    publish_one("/bin/date", (u64)(&user_date_start), (u64)(&user_date_end));
    publish_one("/bin/stat", (u64)(&user_stat_start), (u64)(&user_stat_end));
    publish_one("/bin/nl", (u64)(&user_nl_start), (u64)(&user_nl_end));
    publish_one("/bin/fold", (u64)(&user_fold_start), (u64)(&user_fold_end));
    publish_one("/bin/mktemp", (u64)(&user_mktemp_start), (u64)(&user_mktemp_end));
    publish_one("/bin/rev", (u64)(&user_rev_start), (u64)(&user_rev_end));
    publish_one("/bin/paste", (u64)(&user_paste_start), (u64)(&user_paste_end));
    publish_one("/bin/expand", (u64)(&user_expand_start), (u64)(&user_expand_end));
    publish_one("/bin/unexpand", (u64)(&user_unexpand_start), (u64)(&user_unexpand_end));
    publish_one("/bin/comm", (u64)(&user_comm_start), (u64)(&user_comm_end));
    publish_one("/bin/join", (u64)(&user_join_start), (u64)(&user_join_end));
    publish_one("/bin/split", (u64)(&user_split_start), (u64)(&user_split_end));
    publish_one("/bin/csplit", (u64)(&user_csplit_start), (u64)(&user_csplit_end));
    publish_one("/bin/fmt", (u64)(&user_fmt_start), (u64)(&user_fmt_end));
    publish_one("/bin/which", (u64)(&user_which_start), (u64)(&user_which_end));
    publish_one("/bin/cksum", (u64)(&user_cksum_start), (u64)(&user_cksum_end));
    publish_one("/bin/sum", (u64)(&user_sum_start), (u64)(&user_sum_end));
    publish_one("/bin/strings", (u64)(&user_strings_start), (u64)(&user_strings_end));
    publish_one("/bin/pr", (u64)(&user_pr_start), (u64)(&user_pr_end));
    publish_one("/bin/column", (u64)(&user_column_start), (u64)(&user_column_end));
    publish_one("/bin/md5sum", (u64)(&user_md5sum_start), (u64)(&user_md5sum_end));
    publish_one("/bin/sha1sum", (u64)(&user_sha1sum_start), (u64)(&user_sha1sum_end));
    publish_one("/bin/sha256sum", (u64)(&user_sha256sum_start), (u64)(&user_sha256sum_end));
    publish_one("/bin/base64", (u64)(&user_base64_start), (u64)(&user_base64_end));
    publish_one("/bin/factor", (u64)(&user_factor_start), (u64)(&user_factor_end));
    publish_one("/bin/clear", (u64)(&user_clear_start), (u64)(&user_clear_end));
    publish_one("/bin/tty", (u64)(&user_tty_start), (u64)(&user_tty_end));
    publish_one("/bin/whoami", (u64)(&user_whoami_start), (u64)(&user_whoami_end));
    publish_one("/bin/id", (u64)(&user_id_start), (u64)(&user_id_end));
    publish_one("/bin/groups", (u64)(&user_groups_start), (u64)(&user_groups_end));
    publish_one("/bin/cal", (u64)(&user_cal_start), (u64)(&user_cal_end));
    publish_one("/bin/install", (u64)(&user_install_start), (u64)(&user_install_end));
    publish_one("/bin/chown", (u64)(&user_chown_start), (u64)(&user_chown_end));
    publish_one("/bin/chgrp", (u64)(&user_chgrp_start), (u64)(&user_chgrp_end));
    publish_one("/bin/tac", (u64)(&user_tac_start), (u64)(&user_tac_end));
    publish_one("/bin/nproc", (u64)(&user_nproc_start), (u64)(&user_nproc_end));
    publish_one("/bin/timeout", (u64)(&user_timeout_start), (u64)(&user_timeout_end));
    publish_one("/bin/rmdir", (u64)(&user_rmdir_start), (u64)(&user_rmdir_end));
    publish_one("/bin/unlink", (u64)(&user_unlink_start), (u64)(&user_unlink_end));
    publish_one("/bin/tsort", (u64)(&user_tsort_start), (u64)(&user_tsort_end));
    publish_one("/bin/pathchk", (u64)(&user_pathchk_start), (u64)(&user_pathchk_end));
    publish_one("/bin/logname", (u64)(&user_logname_start), (u64)(&user_logname_end));
    publish_one("/bin/nohup", (u64)(&user_nohup_start), (u64)(&user_nohup_end));
    publish_one("/bin/find", (u64)(&user_find_start), (u64)(&user_find_end));
    publish_one("/bin/du", (u64)(&user_du_start), (u64)(&user_du_end));
    publish_one("/bin/shuf", (u64)(&user_shuf_start), (u64)(&user_shuf_end));
    publish_one("/bin/kill", (u64)(&user_kill_start), (u64)(&user_kill_end));
    publish_one("/bin/diff", (u64)(&user_diff_start), (u64)(&user_diff_end));
    publish_one("/bin/printenv", (u64)(&user_env_start), (u64)(&user_env_end));
    publish_one("/bin/kbuild", (u64)(&user_kbuild_start), (u64)(&user_kbuild_end));
    publish_one("/bin/ldfake", (u64)(&user_ldfake_start), (u64)(&user_ldfake_end));
    publish_one("/bin/dumpbin", (u64)(&user_dumpbin_start), (u64)(&user_dumpbin_end));
    publish_one("/etc/motd",  (u64)(&motd_start),       (u64)(&motd_end));
    publish_one("/etc/resolv.conf", (u64)(&resolv_conf_start), (u64)(&resolv_conf_end));
    publish_one("/etc/hosts", (u64)(&hosts_file_start), (u64)(&hosts_file_end));
    publish_one("/etc/passwd", (u64)(&passwd_file_start), (u64)(&passwd_file_end));
    publish_one("/etc/group", (u64)(&group_file_start), (u64)(&group_file_end));
    /* /proc/self/{status,maps,cmdline,environ} are live synthetic nodes.
     * Empty envp still seeds FAKECC_PKG=/src in the TCB so fakecc getenv
     * via /proc/self/environ keeps working for kbuild. */
    environ_blob_anchor = (u64)(&environ_start) ^ (u64)(&environ_end);
    if (!fs.fs_mkdir("/proc")) {
        kprintf("[FS] FAILED to publish /proc\n");
    }
    if (!fs.fs_mkdir("/proc/self")) {
        kprintf("[FS] FAILED to publish /proc/self\n");
    }

    publish_cpuinfo();

    /* Writable scratch directory for compiler outputs. */
    if (!fs.fs_mkdir("/tmp")) {
        kprintf("[FS] FAILED to publish /tmp\n");
    }
    /* POSIX shm lives as named Ramfs files under /dev/shm (shm_open path). */
    if (!fs.fs_mkdir("/dev")) {
        kprintf("[FS] FAILED to publish /dev\n");
    }
    if (!fs.fs_mkdir("/dev/shm")) {
        kprintf("[FS] FAILED to publish /dev/shm\n");
    }
    if (!fs.fs_publish_chrdevs()) {
        kprintf("[FS] FAILED to publish /dev/{null,zero,tty}\n");
    } else {
        kprintf("[FS] published /dev/null /dev/zero /dev/tty\n");
    }
    if (!fs.fs_publish_procfs()) {
        kprintf("[FS] FAILED to publish /proc/self/{status,maps,cmdline,environ}\n");
    } else {
        kprintf("[FS] published /proc/self/status maps cmdline environ\n");
    }

    for (i = 0; i < m5_blob_count; i++) {
        u64 msize = (u64)m5_blob_table[i].end - (u64)m5_blob_table[i].start;
        /* Same rule as publish_one(): a zero-length staged file is legitimate
         * (an empty /etc/fakecc.flags) and must not be reported as a failure.
         * fs_publish_file rejects size 0, so skip those entries here instead
         * of letting the batch loop report a phantom [FS] FAILED. */
        if (msize == 0) {
            continue;
        }
        if (fs.fs_publish_file(m5_blob_table[i].path,
                              (u64)m5_blob_table[i].start, msize)) {
            ok++;
        } else {
            kprintf("[FS] FAILED to publish %s\n", m5_blob_table[i].path);
        }
    }
    kprintf("[FS] published %u staged bootstrap files\n", ok);
}
