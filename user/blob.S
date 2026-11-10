/* fakeos - embedded userland ELF blobs (Gas AT&T .incbin for fakecc) */
    .section .rodata

/* path actual must include quotes; .incbin gets them from \path */
    .macro BLOB sym path
    .globl \sym\()_start
    .globl \sym\()_end
    .align 8
\sym\()_start:
    .incbin \path
\sym\()_end:
    .endm

    BLOB user_init, "build/user/init.elf"
    BLOB user_sh, "build/user/sh.elf"
    BLOB user_hello, "build/user/hello.elf"
    BLOB user_cat, "build/user/cat.elf"
    BLOB user_ls, "build/user/ls.elf"
    BLOB user_cp, "build/user/cp.elf"
    BLOB user_rm, "build/user/rm.elf"
    BLOB user_touch, "build/user/touch.elf"
    BLOB user_mv, "build/user/mv.elf"
    BLOB user_ping, "build/user/ping.elf"
    BLOB user_udptest, "build/user/udptest.elf"
    BLOB user_nc, "build/user/nc.elf"
    BLOB user_ed, "build/user/ed.elf"
    BLOB user_login, "build/user/login.elf"
    BLOB user_head, "build/user/head.elf"
    BLOB user_wc, "build/user/wc.elf"
    BLOB user_getent, "build/user/getent.elf"
    BLOB user_tail, "build/user/tail.elf"
    BLOB user_grep, "build/user/grep.elf"
    BLOB user_sed, "build/user/sed.elf"
    BLOB user_printf, "build/user/printf.elf"
    BLOB user_tee, "build/user/tee.elf"
    BLOB user_basename, "build/user/basename.elf"
    BLOB user_dirname, "build/user/dirname.elf"
    BLOB user_true, "build/user/true.elf"
    BLOB user_false, "build/user/false.elf"
    BLOB user_seq, "build/user/seq.elf"
    BLOB user_cut, "build/user/cut.elf"
    BLOB user_tr, "build/user/tr.elf"
    BLOB user_test, "build/user/test.elf"
    BLOB user_uniq, "build/user/uniq.elf"
    BLOB user_sort, "build/user/sort.elf"
    BLOB user_yes, "build/user/yes.elf"
    BLOB user_od, "build/user/od.elf"
    BLOB user_xargs, "build/user/xargs.elf"
    BLOB user_cmp, "build/user/cmp.elf"
    BLOB user_dd, "build/user/dd.elf"
    BLOB user_expr, "build/user/expr.elf"
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
    BLOB user_ps, "build/user/ps.elf"
    BLOB user_free, "build/user/free.elf"
    BLOB user_timeout, "build/user/timeout.elf"
    BLOB user_rmdir, "build/user/rmdir.elf"
    BLOB user_unlink, "build/user/unlink.elf"
    BLOB user_tsort, "build/user/tsort.elf"
    BLOB user_pathchk, "build/user/pathchk.elf"
    BLOB user_logname, "build/user/logname.elf"
    BLOB user_nohup, "build/user/nohup.elf"
    BLOB user_find, "build/user/find.elf"
    BLOB user_du, "build/user/du.elf"
    BLOB user_df, "build/user/df.elf"
    BLOB user_shuf, "build/user/shuf.elf"
    BLOB user_kill, "build/user/kill.elf"
    BLOB user_diff, "build/user/diff.elf"
    BLOB user_nice, "build/user/nice.elf"
    BLOB user_arch, "build/user/arch.elf"
    BLOB user_time, "build/user/time.elf"
    BLOB user_watch, "build/user/watch.elf"
    BLOB user_hexdump, "build/user/hexdump.elf"
    BLOB user_xxd, "build/user/xxd.elf"
    BLOB user_file, "build/user/file.elf"
    BLOB user_kbuild, "build/user/kbuild.elf"
    BLOB user_ldfake, "build/user/ldfake.elf"
    BLOB user_dumpbin, "build/user/dumpbin.elf"
    BLOB motd, "user/motd.txt"
    BLOB environ, "user/environ.txt"
    BLOB resolv_conf, "user/resolv.conf"
    BLOB hosts_file, "user/hosts"
    BLOB passwd_file, "user/passwd"
    BLOB group_file, "user/group"

    /* Path strings stay in .rodata; the pointer table goes in .data
     * so fakecc emits R_X86_64_64 relocs (same pattern as m5_blob_table).
     */
path_user_init:
    .byte 47, 98, 105, 110, 47, 105, 110, 105, 116, 0
path_user_sh:
    .byte 47, 98, 105, 110, 47, 115, 104, 0
path_user_hello:
    .byte 47, 98, 105, 110, 47, 104, 101, 108, 108, 111, 0
path_user_cat:
    .byte 47, 98, 105, 110, 47, 99, 97, 116, 0
path_user_ls:
    .byte 47, 98, 105, 110, 47, 108, 115, 0
path_user_cp:
    .byte 47, 98, 105, 110, 47, 99, 112, 0
path_user_rm:
    .byte 47, 98, 105, 110, 47, 114, 109, 0
path_user_touch:
    .byte 47, 98, 105, 110, 47, 116, 111, 117, 99, 104, 0
path_user_mv:
    .byte 47, 98, 105, 110, 47, 109, 118, 0
path_user_ping:
    .byte 47, 98, 105, 110, 47, 112, 105, 110, 103, 0
path_user_udptest:
    .byte 47, 98, 105, 110, 47, 117, 100, 112, 116, 101, 115, 116, 0
path_user_nc:
    .byte 47, 98, 105, 110, 47, 110, 99, 0
path_user_ed:
    .byte 47, 98, 105, 110, 47, 101, 100, 0
path_user_login:
    .byte 47, 98, 105, 110, 47, 108, 111, 103, 105, 110, 0
path_user_head:
    .byte 47, 98, 105, 110, 47, 104, 101, 97, 100, 0
path_user_wc:
    .byte 47, 98, 105, 110, 47, 119, 99, 0
path_user_getent:
    .byte 47, 98, 105, 110, 47, 103, 101, 116, 101, 110, 116, 0
path_user_tail:
    .byte 47, 98, 105, 110, 47, 116, 97, 105, 108, 0
path_user_grep:
    .byte 47, 98, 105, 110, 47, 103, 114, 101, 112, 0
path_user_sed:
    .byte 47, 98, 105, 110, 47, 115, 101, 100, 0
path_user_printf:
    .byte 47, 98, 105, 110, 47, 112, 114, 105, 110, 116, 102, 0
path_user_tee:
    .byte 47, 98, 105, 110, 47, 116, 101, 101, 0
path_user_basename:
    .byte 47, 98, 105, 110, 47, 98, 97, 115, 101, 110, 97, 109, 101, 0
path_user_dirname:
    .byte 47, 98, 105, 110, 47, 100, 105, 114, 110, 97, 109, 101, 0
path_user_true:
    .byte 47, 98, 105, 110, 47, 116, 114, 117, 101, 0
path_user_false:
    .byte 47, 98, 105, 110, 47, 102, 97, 108, 115, 101, 0
path_user_seq:
    .byte 47, 98, 105, 110, 47, 115, 101, 113, 0
path_user_cut:
    .byte 47, 98, 105, 110, 47, 99, 117, 116, 0
path_user_tr:
    .byte 47, 98, 105, 110, 47, 116, 114, 0
path_user_test:
    .byte 47, 98, 105, 110, 47, 116, 101, 115, 116, 0
path_lbrack:
    .byte 47, 98, 105, 110, 47, 91, 0
path_user_uniq:
    .byte 47, 98, 105, 110, 47, 117, 110, 105, 113, 0
path_user_sort:
    .byte 47, 98, 105, 110, 47, 115, 111, 114, 116, 0
path_user_yes:
    .byte 47, 98, 105, 110, 47, 121, 101, 115, 0
path_user_od:
    .byte 47, 98, 105, 110, 47, 111, 100, 0
path_user_xargs:
    .byte 47, 98, 105, 110, 47, 120, 97, 114, 103, 115, 0
path_user_cmp:
    .byte 47, 98, 105, 110, 47, 99, 109, 112, 0
path_user_dd:
    .byte 47, 98, 105, 110, 47, 100, 100, 0
path_user_expr:
    .byte 47, 98, 105, 110, 47, 101, 120, 112, 114, 0
path_user_hostname:
    .byte 47, 98, 105, 110, 47, 104, 111, 115, 116, 110, 97, 109, 101, 0
path_user_echo:
    .byte 47, 98, 105, 110, 47, 101, 99, 104, 111, 0
path_user_sleep:
    .byte 47, 98, 105, 110, 47, 115, 108, 101, 101, 112, 0
path_user_truncate:
    .byte 47, 98, 105, 110, 47, 116, 114, 117, 110, 99, 97, 116, 101, 0
path_user_mkdir:
    .byte 47, 98, 105, 110, 47, 109, 107, 100, 105, 114, 0
path_user_pwd:
    .byte 47, 98, 105, 110, 47, 112, 119, 100, 0
path_user_ln:
    .byte 47, 98, 105, 110, 47, 108, 110, 0
path_user_chmod:
    .byte 47, 98, 105, 110, 47, 99, 104, 109, 111, 100, 0
path_user_uname:
    .byte 47, 98, 105, 110, 47, 117, 110, 97, 109, 101, 0
path_user_env:
    .byte 47, 98, 105, 110, 47, 101, 110, 118, 0
path_printenv:
    .byte 47, 98, 105, 110, 47, 112, 114, 105, 110, 116, 101, 110, 118, 0
path_user_sync:
    .byte 47, 98, 105, 110, 47, 115, 121, 110, 99, 0
path_user_readlink:
    .byte 47, 98, 105, 110, 47, 114, 101, 97, 100, 108, 105, 110, 107, 0
path_user_realpath:
    .byte 47, 98, 105, 110, 47, 114, 101, 97, 108, 112, 97, 116, 104, 0
path_user_date:
    .byte 47, 98, 105, 110, 47, 100, 97, 116, 101, 0
path_user_stat:
    .byte 47, 98, 105, 110, 47, 115, 116, 97, 116, 0
path_user_nl:
    .byte 47, 98, 105, 110, 47, 110, 108, 0
path_user_fold:
    .byte 47, 98, 105, 110, 47, 102, 111, 108, 100, 0
path_user_mktemp:
    .byte 47, 98, 105, 110, 47, 109, 107, 116, 101, 109, 112, 0
path_user_rev:
    .byte 47, 98, 105, 110, 47, 114, 101, 118, 0
path_user_paste:
    .byte 47, 98, 105, 110, 47, 112, 97, 115, 116, 101, 0
path_user_expand:
    .byte 47, 98, 105, 110, 47, 101, 120, 112, 97, 110, 100, 0
path_user_unexpand:
    .byte 47, 98, 105, 110, 47, 117, 110, 101, 120, 112, 97, 110, 100, 0
path_user_comm:
    .byte 47, 98, 105, 110, 47, 99, 111, 109, 109, 0
path_user_join:
    .byte 47, 98, 105, 110, 47, 106, 111, 105, 110, 0
path_user_split:
    .byte 47, 98, 105, 110, 47, 115, 112, 108, 105, 116, 0
path_user_csplit:
    .byte 47, 98, 105, 110, 47, 99, 115, 112, 108, 105, 116, 0
path_user_fmt:
    .byte 47, 98, 105, 110, 47, 102, 109, 116, 0
path_user_which:
    .byte 47, 98, 105, 110, 47, 119, 104, 105, 99, 104, 0
path_user_cksum:
    .byte 47, 98, 105, 110, 47, 99, 107, 115, 117, 109, 0
path_user_sum:
    .byte 47, 98, 105, 110, 47, 115, 117, 109, 0
path_user_strings:
    .byte 47, 98, 105, 110, 47, 115, 116, 114, 105, 110, 103, 115, 0
path_user_pr:
    .byte 47, 98, 105, 110, 47, 112, 114, 0
path_user_column:
    .byte 47, 98, 105, 110, 47, 99, 111, 108, 117, 109, 110, 0
path_user_md5sum:
    .byte 47, 98, 105, 110, 47, 109, 100, 53, 115, 117, 109, 0
path_user_sha1sum:
    .byte 47, 98, 105, 110, 47, 115, 104, 97, 49, 115, 117, 109, 0
path_user_sha256sum:
    .byte 47, 98, 105, 110, 47, 115, 104, 97, 50, 53, 54, 115, 117, 109, 0
path_user_base64:
    .byte 47, 98, 105, 110, 47, 98, 97, 115, 101, 54, 52, 0
path_user_factor:
    .byte 47, 98, 105, 110, 47, 102, 97, 99, 116, 111, 114, 0
path_user_clear:
    .byte 47, 98, 105, 110, 47, 99, 108, 101, 97, 114, 0
path_user_reset:
    .byte 47, 98, 105, 110, 47, 114, 101, 115, 101, 116, 0
path_user_stty:
    .byte 47, 98, 105, 110, 47, 115, 116, 116, 121, 0
path_user_tput:
    .byte 47, 98, 105, 110, 47, 116, 112, 117, 116, 0
path_user_tty:
    .byte 47, 98, 105, 110, 47, 116, 116, 121, 0
path_user_whoami:
    .byte 47, 98, 105, 110, 47, 119, 104, 111, 97, 109, 105, 0
path_user_id:
    .byte 47, 98, 105, 110, 47, 105, 100, 0
path_user_groups:
    .byte 47, 98, 105, 110, 47, 103, 114, 111, 117, 112, 115, 0
path_user_cal:
    .byte 47, 98, 105, 110, 47, 99, 97, 108, 0
path_user_install:
    .byte 47, 98, 105, 110, 47, 105, 110, 115, 116, 97, 108, 108, 0
path_user_chown:
    .byte 47, 98, 105, 110, 47, 99, 104, 111, 119, 110, 0
path_user_chgrp:
    .byte 47, 98, 105, 110, 47, 99, 104, 103, 114, 112, 0
path_user_tac:
    .byte 47, 98, 105, 110, 47, 116, 97, 99, 0
path_user_nproc:
    .byte 47, 98, 105, 110, 47, 110, 112, 114, 111, 99, 0
path_user_ps:
    .byte 47, 98, 105, 110, 47, 112, 115, 0
path_user_free:
    .byte 47, 98, 105, 110, 47, 102, 114, 101, 101, 0
path_user_timeout:
    .byte 47, 98, 105, 110, 47, 116, 105, 109, 101, 111, 117, 116, 0
path_user_rmdir:
    .byte 47, 98, 105, 110, 47, 114, 109, 100, 105, 114, 0
path_user_unlink:
    .byte 47, 98, 105, 110, 47, 117, 110, 108, 105, 110, 107, 0
path_user_tsort:
    .byte 47, 98, 105, 110, 47, 116, 115, 111, 114, 116, 0
path_user_pathchk:
    .byte 47, 98, 105, 110, 47, 112, 97, 116, 104, 99, 104, 107, 0
path_user_logname:
    .byte 47, 98, 105, 110, 47, 108, 111, 103, 110, 97, 109, 101, 0
path_user_nohup:
    .byte 47, 98, 105, 110, 47, 110, 111, 104, 117, 112, 0
path_user_find:
    .byte 47, 98, 105, 110, 47, 102, 105, 110, 100, 0
path_user_du:
    .byte 47, 98, 105, 110, 47, 100, 117, 0
path_user_df:
    .byte 47, 98, 105, 110, 47, 100, 102, 0
path_user_shuf:
    .byte 47, 98, 105, 110, 47, 115, 104, 117, 102, 0
path_user_kill:
    .byte 47, 98, 105, 110, 47, 107, 105, 108, 108, 0
path_user_diff:
    .byte 47, 98, 105, 110, 47, 100, 105, 102, 102, 0
path_user_nice:
    .byte 47, 98, 105, 110, 47, 110, 105, 99, 101, 0
path_user_arch:
    .byte 47, 98, 105, 110, 47, 97, 114, 99, 104, 0
path_user_time:
    .byte 47, 98, 105, 110, 47, 116, 105, 109, 101, 0
path_user_watch:
    .byte 47, 98, 105, 110, 47, 119, 97, 116, 99, 104, 0
path_user_hexdump:
    .byte 47, 98, 105, 110, 47, 104, 101, 120, 100, 117, 109, 112, 0
path_user_xxd:
    .byte 47, 98, 105, 110, 47, 120, 120, 100, 0
path_user_file:
    .byte 47, 98, 105, 110, 47, 102, 105, 108, 101, 0
path_user_kbuild:
    .byte 47, 98, 105, 110, 47, 107, 98, 117, 105, 108, 100, 0
path_user_ldfake:
    .byte 47, 98, 105, 110, 47, 108, 100, 102, 97, 107, 101, 0
path_user_dumpbin:
    .byte 47, 98, 105, 110, 47, 100, 117, 109, 112, 98, 105, 110, 0
path_motd:
    .byte 47, 101, 116, 99, 47, 109, 111, 116, 100, 0
path_resolv_conf:
    .byte 47, 101, 116, 99, 47, 114, 101, 115, 111, 108, 118, 46, 99, 111, 110, 102, 0
path_hosts_file:
    .byte 47, 101, 116, 99, 47, 104, 111, 115, 116, 115, 0
path_passwd_file:
    .byte 47, 101, 116, 99, 47, 112, 97, 115, 115, 119, 100, 0
path_group_file:
    .byte 47, 101, 116, 99, 47, 103, 114, 111, 117, 112, 0

    .section .data
    /* Packed publish table: path/start/end so rootfs only needs
     * user_blob_table / user_blob_count GOT entries.
     * fakecc --kernel-multiboot currently caps the GOT at 256.
     */
    .align 8
    .globl user_blob_table
    .globl user_blob_count
user_blob_table:
    .quad path_user_init, user_init_start, user_init_end
    .quad path_user_sh, user_sh_start, user_sh_end
    .quad path_user_hello, user_hello_start, user_hello_end
    .quad path_user_cat, user_cat_start, user_cat_end
    .quad path_user_ls, user_ls_start, user_ls_end
    .quad path_user_cp, user_cp_start, user_cp_end
    .quad path_user_rm, user_rm_start, user_rm_end
    .quad path_user_touch, user_touch_start, user_touch_end
    .quad path_user_mv, user_mv_start, user_mv_end
    .quad path_user_ping, user_ping_start, user_ping_end
    .quad path_user_udptest, user_udptest_start, user_udptest_end
    .quad path_user_nc, user_nc_start, user_nc_end
    .quad path_user_ed, user_ed_start, user_ed_end
    .quad path_user_login, user_login_start, user_login_end
    .quad path_user_head, user_head_start, user_head_end
    .quad path_user_wc, user_wc_start, user_wc_end
    .quad path_user_getent, user_getent_start, user_getent_end
    .quad path_user_tail, user_tail_start, user_tail_end
    .quad path_user_grep, user_grep_start, user_grep_end
    .quad path_user_sed, user_sed_start, user_sed_end
    .quad path_user_printf, user_printf_start, user_printf_end
    .quad path_user_tee, user_tee_start, user_tee_end
    .quad path_user_basename, user_basename_start, user_basename_end
    .quad path_user_dirname, user_dirname_start, user_dirname_end
    .quad path_user_true, user_true_start, user_true_end
    .quad path_user_false, user_false_start, user_false_end
    .quad path_user_seq, user_seq_start, user_seq_end
    .quad path_user_cut, user_cut_start, user_cut_end
    .quad path_user_tr, user_tr_start, user_tr_end
    .quad path_user_test, user_test_start, user_test_end
    .quad path_lbrack, user_test_start, user_test_end
    .quad path_user_uniq, user_uniq_start, user_uniq_end
    .quad path_user_sort, user_sort_start, user_sort_end
    .quad path_user_yes, user_yes_start, user_yes_end
    .quad path_user_od, user_od_start, user_od_end
    .quad path_user_xargs, user_xargs_start, user_xargs_end
    .quad path_user_cmp, user_cmp_start, user_cmp_end
    .quad path_user_dd, user_dd_start, user_dd_end
    .quad path_user_expr, user_expr_start, user_expr_end
    .quad path_user_hostname, user_hostname_start, user_hostname_end
    .quad path_user_echo, user_echo_start, user_echo_end
    .quad path_user_sleep, user_sleep_start, user_sleep_end
    .quad path_user_truncate, user_truncate_start, user_truncate_end
    .quad path_user_mkdir, user_mkdir_start, user_mkdir_end
    .quad path_user_pwd, user_pwd_start, user_pwd_end
    .quad path_user_ln, user_ln_start, user_ln_end
    .quad path_user_chmod, user_chmod_start, user_chmod_end
    .quad path_user_uname, user_uname_start, user_uname_end
    .quad path_user_env, user_env_start, user_env_end
    .quad path_printenv, user_env_start, user_env_end
    .quad path_user_sync, user_sync_start, user_sync_end
    .quad path_user_readlink, user_readlink_start, user_readlink_end
    .quad path_user_realpath, user_realpath_start, user_realpath_end
    .quad path_user_date, user_date_start, user_date_end
    .quad path_user_stat, user_stat_start, user_stat_end
    .quad path_user_nl, user_nl_start, user_nl_end
    .quad path_user_fold, user_fold_start, user_fold_end
    .quad path_user_mktemp, user_mktemp_start, user_mktemp_end
    .quad path_user_rev, user_rev_start, user_rev_end
    .quad path_user_paste, user_paste_start, user_paste_end
    .quad path_user_expand, user_expand_start, user_expand_end
    .quad path_user_unexpand, user_unexpand_start, user_unexpand_end
    .quad path_user_comm, user_comm_start, user_comm_end
    .quad path_user_join, user_join_start, user_join_end
    .quad path_user_split, user_split_start, user_split_end
    .quad path_user_csplit, user_csplit_start, user_csplit_end
    .quad path_user_fmt, user_fmt_start, user_fmt_end
    .quad path_user_which, user_which_start, user_which_end
    .quad path_user_cksum, user_cksum_start, user_cksum_end
    .quad path_user_sum, user_sum_start, user_sum_end
    .quad path_user_strings, user_strings_start, user_strings_end
    .quad path_user_pr, user_pr_start, user_pr_end
    .quad path_user_column, user_column_start, user_column_end
    .quad path_user_md5sum, user_md5sum_start, user_md5sum_end
    .quad path_user_sha1sum, user_sha1sum_start, user_sha1sum_end
    .quad path_user_sha256sum, user_sha256sum_start, user_sha256sum_end
    .quad path_user_base64, user_base64_start, user_base64_end
    .quad path_user_factor, user_factor_start, user_factor_end
    .quad path_user_clear, user_clear_start, user_clear_end
    .quad path_user_reset, user_reset_start, user_reset_end
    .quad path_user_stty, user_stty_start, user_stty_end
    .quad path_user_tput, user_tput_start, user_tput_end
    .quad path_user_tty, user_tty_start, user_tty_end
    .quad path_user_whoami, user_whoami_start, user_whoami_end
    .quad path_user_id, user_id_start, user_id_end
    .quad path_user_groups, user_groups_start, user_groups_end
    .quad path_user_cal, user_cal_start, user_cal_end
    .quad path_user_install, user_install_start, user_install_end
    .quad path_user_chown, user_chown_start, user_chown_end
    .quad path_user_chgrp, user_chgrp_start, user_chgrp_end
    .quad path_user_tac, user_tac_start, user_tac_end
    .quad path_user_nproc, user_nproc_start, user_nproc_end
    .quad path_user_ps, user_ps_start, user_ps_end
    .quad path_user_free, user_free_start, user_free_end
    .quad path_user_timeout, user_timeout_start, user_timeout_end
    .quad path_user_rmdir, user_rmdir_start, user_rmdir_end
    .quad path_user_unlink, user_unlink_start, user_unlink_end
    .quad path_user_tsort, user_tsort_start, user_tsort_end
    .quad path_user_pathchk, user_pathchk_start, user_pathchk_end
    .quad path_user_logname, user_logname_start, user_logname_end
    .quad path_user_nohup, user_nohup_start, user_nohup_end
    .quad path_user_find, user_find_start, user_find_end
    .quad path_user_du, user_du_start, user_du_end
    .quad path_user_df, user_df_start, user_df_end
    .quad path_user_shuf, user_shuf_start, user_shuf_end
    .quad path_user_kill, user_kill_start, user_kill_end
    .quad path_user_diff, user_diff_start, user_diff_end
    .quad path_user_nice, user_nice_start, user_nice_end
    .quad path_user_arch, user_arch_start, user_arch_end
    .quad path_user_time, user_time_start, user_time_end
    .quad path_user_watch, user_watch_start, user_watch_end
    .quad path_user_hexdump, user_hexdump_start, user_hexdump_end
    .quad path_user_xxd, user_xxd_start, user_xxd_end
    .quad path_user_file, user_file_start, user_file_end
    .quad path_user_kbuild, user_kbuild_start, user_kbuild_end
    .quad path_user_ldfake, user_ldfake_start, user_ldfake_end
    .quad path_user_dumpbin, user_dumpbin_start, user_dumpbin_end
    .quad path_motd, motd_start, motd_end
    .quad path_resolv_conf, resolv_conf_start, resolv_conf_end
    .quad path_hosts_file, hosts_file_start, hosts_file_end
    .quad path_passwd_file, passwd_file_start, passwd_file_end
    .quad path_group_file, group_file_start, group_file_end
user_blob_table_end:
    .align 8
user_blob_count:
    .quad 122
