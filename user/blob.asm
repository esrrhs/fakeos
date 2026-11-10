; ==============================================================================
; fakeos - embedded rootfs images (built from user/ at user virtual addresses)
;
; Six static ELF user programs plus the /etc/motd text. rootfs_publish() in
; kernel/rootfs.c releases them into Ramfs at boot. All blobs live in the
; kernel .rodata and are mapped read-only into the higher-half image.
; ==============================================================================

[bits 64]

%macro BLOB 2
global %1%+_start
global %1%+_end
align 8
%1%+_start:
    incbin %2
%1%+_end:
%endmacro

section .rodata

BLOB user_init,  "build/user/init.elf"
BLOB user_sh,    "build/user/sh.elf"
BLOB user_hello, "build/user/hello.elf"
BLOB user_cat,   "build/user/cat.elf"
BLOB user_ls,    "build/user/ls.elf"
BLOB user_cp,    "build/user/cp.elf"
BLOB user_rm,    "build/user/rm.elf"
BLOB user_touch, "build/user/touch.elf"
BLOB user_mv,    "build/user/mv.elf"
BLOB user_ping,  "build/user/ping.elf"
BLOB user_udptest, "build/user/udptest.elf"
BLOB user_nc,    "build/user/nc.elf"
BLOB user_ed,    "build/user/ed.elf"
BLOB user_login, "build/user/login.elf"
BLOB user_head,  "build/user/head.elf"
BLOB user_wc,    "build/user/wc.elf"
BLOB user_getent, "build/user/getent.elf"
BLOB user_tail,  "build/user/tail.elf"
BLOB user_grep,  "build/user/grep.elf"
BLOB user_sed,   "build/user/sed.elf"
BLOB user_printf,"build/user/printf.elf"
BLOB user_tee,   "build/user/tee.elf"
BLOB user_basename, "build/user/basename.elf"
BLOB user_dirname, "build/user/dirname.elf"
BLOB user_true,  "build/user/true.elf"
BLOB user_false, "build/user/false.elf"
BLOB user_seq,   "build/user/seq.elf"
BLOB user_cut,   "build/user/cut.elf"
BLOB user_tr,    "build/user/tr.elf"
BLOB user_test, "build/user/test.elf"
BLOB user_uniq,  "build/user/uniq.elf"
BLOB user_sort,  "build/user/sort.elf"
BLOB user_yes,   "build/user/yes.elf"
BLOB user_od,    "build/user/od.elf"
BLOB user_xargs, "build/user/xargs.elf"
BLOB user_cmp,   "build/user/cmp.elf"
BLOB user_dd,    "build/user/dd.elf"
BLOB user_expr,  "build/user/expr.elf"
BLOB user_hostname, "build/user/hostname.elf"
BLOB user_echo, "build/user/echo.elf"
BLOB user_sleep, "build/user/sleep.elf"
BLOB user_truncate, "build/user/truncate.elf"
BLOB user_mkdir, "build/user/mkdir.elf"
BLOB user_pwd, "build/user/pwd.elf"
BLOB user_ln, "build/user/ln.elf"
BLOB user_chmod, "build/user/chmod.elf"
BLOB user_uname, "build/user/uname.elf"
BLOB user_env, "build/user/env.elf"
BLOB user_sync, "build/user/sync.elf"
BLOB user_readlink, "build/user/readlink.elf"
BLOB user_realpath, "build/user/realpath.elf"
BLOB user_date, "build/user/date.elf"
BLOB user_stat, "build/user/stat.elf"
BLOB user_nl, "build/user/nl.elf"
BLOB user_fold, "build/user/fold.elf"
BLOB user_mktemp, "build/user/mktemp.elf"
BLOB user_rev, "build/user/rev.elf"
BLOB user_paste, "build/user/paste.elf"
BLOB user_expand, "build/user/expand.elf"
BLOB user_unexpand, "build/user/unexpand.elf"
BLOB user_comm, "build/user/comm.elf"
BLOB user_join, "build/user/join.elf"
BLOB user_split, "build/user/split.elf"
BLOB user_csplit, "build/user/csplit.elf"
BLOB user_fmt, "build/user/fmt.elf"
BLOB user_which, "build/user/which.elf"
BLOB user_cksum, "build/user/cksum.elf"
BLOB user_sum, "build/user/sum.elf"
BLOB user_strings, "build/user/strings.elf"
BLOB user_pr, "build/user/pr.elf"
BLOB user_column, "build/user/column.elf"
BLOB user_md5sum, "build/user/md5sum.elf"
BLOB user_sha1sum, "build/user/sha1sum.elf"
BLOB user_sha256sum, "build/user/sha256sum.elf"
BLOB user_base64, "build/user/base64.elf"
BLOB user_factor, "build/user/factor.elf"
BLOB user_clear, "build/user/clear.elf"
BLOB user_reset, "build/user/reset.elf"
BLOB user_stty, "build/user/stty.elf"
BLOB user_tput, "build/user/tput.elf"
BLOB user_tty, "build/user/tty.elf"
BLOB user_whoami, "build/user/whoami.elf"
BLOB user_id, "build/user/id.elf"
BLOB user_groups, "build/user/groups.elf"
BLOB user_cal, "build/user/cal.elf"
BLOB user_install, "build/user/install.elf"
BLOB user_chown, "build/user/chown.elf"
BLOB user_chgrp, "build/user/chgrp.elf"
BLOB user_tac, "build/user/tac.elf"
BLOB user_nproc, "build/user/nproc.elf"
BLOB user_ps,    "build/user/ps.elf"
BLOB user_free,  "build/user/free.elf"
BLOB user_timeout, "build/user/timeout.elf"
BLOB user_rmdir, "build/user/rmdir.elf"
BLOB user_unlink, "build/user/unlink.elf"
BLOB user_tsort, "build/user/tsort.elf"
BLOB user_pathchk, "build/user/pathchk.elf"
BLOB user_logname, "build/user/logname.elf"
BLOB user_nohup, "build/user/nohup.elf"
BLOB user_find,  "build/user/find.elf"
BLOB user_du,    "build/user/du.elf"
BLOB user_df,    "build/user/df.elf"
BLOB user_shuf,  "build/user/shuf.elf"
BLOB user_kill,  "build/user/kill.elf"
BLOB user_diff,  "build/user/diff.elf"
BLOB user_nice,  "build/user/nice.elf"
BLOB user_arch,  "build/user/arch.elf"
BLOB user_time,  "build/user/time.elf"
BLOB user_watch, "build/user/watch.elf"
BLOB user_hexdump, "build/user/hexdump.elf"
BLOB user_xxd, "build/user/xxd.elf"
BLOB user_file, "build/user/file.elf"
BLOB user_kbuild, "build/user/kbuild.elf"
BLOB user_ldfake, "build/user/ldfake.elf"
BLOB user_dumpbin, "build/user/dumpbin.elf"
BLOB motd,       "user/motd.txt"
BLOB environ,    "user/environ.txt"
BLOB resolv_conf,"user/resolv.conf"
BLOB hosts_file, "user/hosts"
BLOB passwd_file,"user/passwd"
BLOB group_file, "user/group"

; Path strings stay in .rodata; the pointer table goes in .data
; so fakecc emits R_X86_64_64 relocs (same pattern as m5_blob_table).
path_user_init:
    db "/bin/init", 0
path_user_sh:
    db "/bin/sh", 0
path_user_hello:
    db "/bin/hello", 0
path_user_cat:
    db "/bin/cat", 0
path_user_ls:
    db "/bin/ls", 0
path_user_cp:
    db "/bin/cp", 0
path_user_rm:
    db "/bin/rm", 0
path_user_touch:
    db "/bin/touch", 0
path_user_mv:
    db "/bin/mv", 0
path_user_ping:
    db "/bin/ping", 0
path_user_udptest:
    db "/bin/udptest", 0
path_user_nc:
    db "/bin/nc", 0
path_user_ed:
    db "/bin/ed", 0
path_user_login:
    db "/bin/login", 0
path_user_head:
    db "/bin/head", 0
path_user_wc:
    db "/bin/wc", 0
path_user_getent:
    db "/bin/getent", 0
path_user_tail:
    db "/bin/tail", 0
path_user_grep:
    db "/bin/grep", 0
path_user_sed:
    db "/bin/sed", 0
path_user_printf:
    db "/bin/printf", 0
path_user_tee:
    db "/bin/tee", 0
path_user_basename:
    db "/bin/basename", 0
path_user_dirname:
    db "/bin/dirname", 0
path_user_true:
    db "/bin/true", 0
path_user_false:
    db "/bin/false", 0
path_user_seq:
    db "/bin/seq", 0
path_user_cut:
    db "/bin/cut", 0
path_user_tr:
    db "/bin/tr", 0
path_user_test:
    db "/bin/test", 0
path_lbrack:
    db "/bin/[", 0
path_user_uniq:
    db "/bin/uniq", 0
path_user_sort:
    db "/bin/sort", 0
path_user_yes:
    db "/bin/yes", 0
path_user_od:
    db "/bin/od", 0
path_user_xargs:
    db "/bin/xargs", 0
path_user_cmp:
    db "/bin/cmp", 0
path_user_dd:
    db "/bin/dd", 0
path_user_expr:
    db "/bin/expr", 0
path_user_hostname:
    db "/bin/hostname", 0
path_user_echo:
    db "/bin/echo", 0
path_user_sleep:
    db "/bin/sleep", 0
path_user_truncate:
    db "/bin/truncate", 0
path_user_mkdir:
    db "/bin/mkdir", 0
path_user_pwd:
    db "/bin/pwd", 0
path_user_ln:
    db "/bin/ln", 0
path_user_chmod:
    db "/bin/chmod", 0
path_user_uname:
    db "/bin/uname", 0
path_user_env:
    db "/bin/env", 0
path_printenv:
    db "/bin/printenv", 0
path_user_sync:
    db "/bin/sync", 0
path_user_readlink:
    db "/bin/readlink", 0
path_user_realpath:
    db "/bin/realpath", 0
path_user_date:
    db "/bin/date", 0
path_user_stat:
    db "/bin/stat", 0
path_user_nl:
    db "/bin/nl", 0
path_user_fold:
    db "/bin/fold", 0
path_user_mktemp:
    db "/bin/mktemp", 0
path_user_rev:
    db "/bin/rev", 0
path_user_paste:
    db "/bin/paste", 0
path_user_expand:
    db "/bin/expand", 0
path_user_unexpand:
    db "/bin/unexpand", 0
path_user_comm:
    db "/bin/comm", 0
path_user_join:
    db "/bin/join", 0
path_user_split:
    db "/bin/split", 0
path_user_csplit:
    db "/bin/csplit", 0
path_user_fmt:
    db "/bin/fmt", 0
path_user_which:
    db "/bin/which", 0
path_user_cksum:
    db "/bin/cksum", 0
path_user_sum:
    db "/bin/sum", 0
path_user_strings:
    db "/bin/strings", 0
path_user_pr:
    db "/bin/pr", 0
path_user_column:
    db "/bin/column", 0
path_user_md5sum:
    db "/bin/md5sum", 0
path_user_sha1sum:
    db "/bin/sha1sum", 0
path_user_sha256sum:
    db "/bin/sha256sum", 0
path_user_base64:
    db "/bin/base64", 0
path_user_factor:
    db "/bin/factor", 0
path_user_clear:
    db "/bin/clear", 0
path_user_reset:
    db "/bin/reset", 0
path_user_stty:
    db "/bin/stty", 0
path_user_tput:
    db "/bin/tput", 0
path_user_tty:
    db "/bin/tty", 0
path_user_whoami:
    db "/bin/whoami", 0
path_user_id:
    db "/bin/id", 0
path_user_groups:
    db "/bin/groups", 0
path_user_cal:
    db "/bin/cal", 0
path_user_install:
    db "/bin/install", 0
path_user_chown:
    db "/bin/chown", 0
path_user_chgrp:
    db "/bin/chgrp", 0
path_user_tac:
    db "/bin/tac", 0
path_user_nproc:
    db "/bin/nproc", 0
path_user_ps:
    db "/bin/ps", 0
path_user_free:
    db "/bin/free", 0
path_user_timeout:
    db "/bin/timeout", 0
path_user_rmdir:
    db "/bin/rmdir", 0
path_user_unlink:
    db "/bin/unlink", 0
path_user_tsort:
    db "/bin/tsort", 0
path_user_pathchk:
    db "/bin/pathchk", 0
path_user_logname:
    db "/bin/logname", 0
path_user_nohup:
    db "/bin/nohup", 0
path_user_find:
    db "/bin/find", 0
path_user_du:
    db "/bin/du", 0
path_user_df:
    db "/bin/df", 0
path_user_shuf:
    db "/bin/shuf", 0
path_user_kill:
    db "/bin/kill", 0
path_user_diff:
    db "/bin/diff", 0
path_user_nice:
    db "/bin/nice", 0
path_user_arch:
    db "/bin/arch", 0
path_user_time:
    db "/bin/time", 0
path_user_watch:
    db "/bin/watch", 0
path_user_hexdump:
    db "/bin/hexdump", 0
path_user_xxd:
    db "/bin/xxd", 0
path_user_file:
    db "/bin/file", 0
path_user_kbuild:
    db "/bin/kbuild", 0
path_user_ldfake:
    db "/bin/ldfake", 0
path_user_dumpbin:
    db "/bin/dumpbin", 0
path_motd:
    db "/etc/motd", 0
path_resolv_conf:
    db "/etc/resolv.conf", 0
path_hosts_file:
    db "/etc/hosts", 0
path_passwd_file:
    db "/etc/passwd", 0
path_group_file:
    db "/etc/group", 0

section .data
; Packed publish table: path/start/end so rootfs only needs
; user_blob_table / user_blob_count GOT entries.
; fakecc --kernel-multiboot currently caps the GOT at 256.
align 8
global user_blob_table
global user_blob_count
user_blob_table:
    dq path_user_init, user_init_start, user_init_end
    dq path_user_sh, user_sh_start, user_sh_end
    dq path_user_hello, user_hello_start, user_hello_end
    dq path_user_cat, user_cat_start, user_cat_end
    dq path_user_ls, user_ls_start, user_ls_end
    dq path_user_cp, user_cp_start, user_cp_end
    dq path_user_rm, user_rm_start, user_rm_end
    dq path_user_touch, user_touch_start, user_touch_end
    dq path_user_mv, user_mv_start, user_mv_end
    dq path_user_ping, user_ping_start, user_ping_end
    dq path_user_udptest, user_udptest_start, user_udptest_end
    dq path_user_nc, user_nc_start, user_nc_end
    dq path_user_ed, user_ed_start, user_ed_end
    dq path_user_login, user_login_start, user_login_end
    dq path_user_head, user_head_start, user_head_end
    dq path_user_wc, user_wc_start, user_wc_end
    dq path_user_getent, user_getent_start, user_getent_end
    dq path_user_tail, user_tail_start, user_tail_end
    dq path_user_grep, user_grep_start, user_grep_end
    dq path_user_sed, user_sed_start, user_sed_end
    dq path_user_printf, user_printf_start, user_printf_end
    dq path_user_tee, user_tee_start, user_tee_end
    dq path_user_basename, user_basename_start, user_basename_end
    dq path_user_dirname, user_dirname_start, user_dirname_end
    dq path_user_true, user_true_start, user_true_end
    dq path_user_false, user_false_start, user_false_end
    dq path_user_seq, user_seq_start, user_seq_end
    dq path_user_cut, user_cut_start, user_cut_end
    dq path_user_tr, user_tr_start, user_tr_end
    dq path_user_test, user_test_start, user_test_end
    dq path_lbrack, user_test_start, user_test_end
    dq path_user_uniq, user_uniq_start, user_uniq_end
    dq path_user_sort, user_sort_start, user_sort_end
    dq path_user_yes, user_yes_start, user_yes_end
    dq path_user_od, user_od_start, user_od_end
    dq path_user_xargs, user_xargs_start, user_xargs_end
    dq path_user_cmp, user_cmp_start, user_cmp_end
    dq path_user_dd, user_dd_start, user_dd_end
    dq path_user_expr, user_expr_start, user_expr_end
    dq path_user_hostname, user_hostname_start, user_hostname_end
    dq path_user_echo, user_echo_start, user_echo_end
    dq path_user_sleep, user_sleep_start, user_sleep_end
    dq path_user_truncate, user_truncate_start, user_truncate_end
    dq path_user_mkdir, user_mkdir_start, user_mkdir_end
    dq path_user_pwd, user_pwd_start, user_pwd_end
    dq path_user_ln, user_ln_start, user_ln_end
    dq path_user_chmod, user_chmod_start, user_chmod_end
    dq path_user_uname, user_uname_start, user_uname_end
    dq path_user_env, user_env_start, user_env_end
    dq path_printenv, user_env_start, user_env_end
    dq path_user_sync, user_sync_start, user_sync_end
    dq path_user_readlink, user_readlink_start, user_readlink_end
    dq path_user_realpath, user_realpath_start, user_realpath_end
    dq path_user_date, user_date_start, user_date_end
    dq path_user_stat, user_stat_start, user_stat_end
    dq path_user_nl, user_nl_start, user_nl_end
    dq path_user_fold, user_fold_start, user_fold_end
    dq path_user_mktemp, user_mktemp_start, user_mktemp_end
    dq path_user_rev, user_rev_start, user_rev_end
    dq path_user_paste, user_paste_start, user_paste_end
    dq path_user_expand, user_expand_start, user_expand_end
    dq path_user_unexpand, user_unexpand_start, user_unexpand_end
    dq path_user_comm, user_comm_start, user_comm_end
    dq path_user_join, user_join_start, user_join_end
    dq path_user_split, user_split_start, user_split_end
    dq path_user_csplit, user_csplit_start, user_csplit_end
    dq path_user_fmt, user_fmt_start, user_fmt_end
    dq path_user_which, user_which_start, user_which_end
    dq path_user_cksum, user_cksum_start, user_cksum_end
    dq path_user_sum, user_sum_start, user_sum_end
    dq path_user_strings, user_strings_start, user_strings_end
    dq path_user_pr, user_pr_start, user_pr_end
    dq path_user_column, user_column_start, user_column_end
    dq path_user_md5sum, user_md5sum_start, user_md5sum_end
    dq path_user_sha1sum, user_sha1sum_start, user_sha1sum_end
    dq path_user_sha256sum, user_sha256sum_start, user_sha256sum_end
    dq path_user_base64, user_base64_start, user_base64_end
    dq path_user_factor, user_factor_start, user_factor_end
    dq path_user_clear, user_clear_start, user_clear_end
    dq path_user_reset, user_reset_start, user_reset_end
    dq path_user_stty, user_stty_start, user_stty_end
    dq path_user_tput, user_tput_start, user_tput_end
    dq path_user_tty, user_tty_start, user_tty_end
    dq path_user_whoami, user_whoami_start, user_whoami_end
    dq path_user_id, user_id_start, user_id_end
    dq path_user_groups, user_groups_start, user_groups_end
    dq path_user_cal, user_cal_start, user_cal_end
    dq path_user_install, user_install_start, user_install_end
    dq path_user_chown, user_chown_start, user_chown_end
    dq path_user_chgrp, user_chgrp_start, user_chgrp_end
    dq path_user_tac, user_tac_start, user_tac_end
    dq path_user_nproc, user_nproc_start, user_nproc_end
    dq path_user_ps, user_ps_start, user_ps_end
    dq path_user_free, user_free_start, user_free_end
    dq path_user_timeout, user_timeout_start, user_timeout_end
    dq path_user_rmdir, user_rmdir_start, user_rmdir_end
    dq path_user_unlink, user_unlink_start, user_unlink_end
    dq path_user_tsort, user_tsort_start, user_tsort_end
    dq path_user_pathchk, user_pathchk_start, user_pathchk_end
    dq path_user_logname, user_logname_start, user_logname_end
    dq path_user_nohup, user_nohup_start, user_nohup_end
    dq path_user_find, user_find_start, user_find_end
    dq path_user_du, user_du_start, user_du_end
    dq path_user_df, user_df_start, user_df_end
    dq path_user_shuf, user_shuf_start, user_shuf_end
    dq path_user_kill, user_kill_start, user_kill_end
    dq path_user_diff, user_diff_start, user_diff_end
    dq path_user_nice, user_nice_start, user_nice_end
    dq path_user_arch, user_arch_start, user_arch_end
    dq path_user_time, user_time_start, user_time_end
    dq path_user_watch, user_watch_start, user_watch_end
    dq path_user_hexdump, user_hexdump_start, user_hexdump_end
    dq path_user_xxd, user_xxd_start, user_xxd_end
    dq path_user_file, user_file_start, user_file_end
    dq path_user_kbuild, user_kbuild_start, user_kbuild_end
    dq path_user_ldfake, user_ldfake_start, user_ldfake_end
    dq path_user_dumpbin, user_dumpbin_start, user_dumpbin_end
    dq path_motd, motd_start, motd_end
    dq path_resolv_conf, resolv_conf_start, resolv_conf_end
    dq path_hosts_file, hosts_file_start, hosts_file_end
    dq path_passwd_file, passwd_file_start, passwd_file_end
    dq path_group_file, group_file_start, group_file_end
user_blob_table_end:
align 8
user_blob_count:
    dq 122

section .note.GNU-stack noalloc noexec nowrite progbits
