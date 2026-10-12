#!/usr/bin/env python3
"""Build the NoCloud seed ISO (volume label CIDATA) for the fakeos UTM guest.

cloud-init's NoCloud datasource looks for a filesystem labelled CIDATA
carrying a `user-data` file. The usual helper for this is cloud-localds,
which is not installed here, and `hdiutil makeiso` does not produce an
image cloud-init can read either. pycdlib writes a spec-compliant
ISO9660, so use that.

Usage: make_cloudinit_seed.py <user-data> <out.iso>
Requires pycdlib (pip install pycdlib).
"""
import os
import sys

import pycdlib


def build_iso(user_data_path, out_path):
    iso = pycdlib.PyCdlib()
    iso.new(vol_ident="CIDATA", joliet=3, rock_ridge="1.09")

    payload = open(user_data_path, "rb").read()
    # Rock Ridge/Joliet carry the long name; ISO9660 level 1 would otherwise
    # mangle "user-data" into "USER-DAT. 1".
    iso.add_fp(io.BytesIO(payload), len(payload),
               "/USERDATA.;1", rr_name="user-data", joliet_path="/user-data")

    iso.write(out_path)
    iso.close()
    print(f"wrote {out_path}: {os.path.getsize(out_path)} bytes (label CIDATA)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_cloudinit_seed.py <user-data> <out.iso>")
    import io
    build_iso(sys.argv[1], sys.argv[2])
