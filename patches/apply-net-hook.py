#!/usr/bin/env python3
"""Add a passive receive hook to ESP-IDF's BLE Mesh core.

Fixtures broadcast their status to the provisioner address and nothing else,
and the mesh stack discards any PDU whose destination is not one of our own
elements before the access layer ever sees it. There is no public API that
exposes "we decrypted a packet from node X", so this inserts one call plus a
weak default into the network receive path.

Idempotent, and safe to re-run after an IDF upgrade:
    python3 patches/apply-net-hook.py [path-to-esp-idf]
"""
import os, sys, shutil

idf = sys.argv[1] if len(sys.argv) > 1 else os.environ.get(
    "IDF_PATH", os.path.expanduser("~/esp/esp-idf"))
net = os.path.join(idf, "components/bt/esp_ble_mesh/core/net.c")

src = open(net).read()
if "amaran_mesh_net_rx" in src:
    print("already patched:", net)
    sys.exit(0)

DECL = """
/* --- amaran: passive receive hook (added by patches/apply-net-hook.py) --- */
__attribute__((weak)) void amaran_mesh_net_rx(uint16_t src, uint16_t dst, uint8_t ctl)
{
    (void)src; (void)dst; (void)ctl;
}

"""
# The weak default goes after the includes: bt_mesh_net_recv() itself is an
# inline wrapper in net.h, so there is no stable function to anchor above.
lines = src.split("\n")
last_inc = max(n for n, l in enumerate(lines) if l.startswith("#include"))
src = "\n".join(lines[:last_inc + 1]) + "\n" + DECL + "\n".join(lines[last_inc + 1:])

CALL_ANCHOR = "    if (bt_mesh_trans_recv(buf, rx) == -EAGAIN) {"
CALL = ("    /* amaran: every decrypted PDU, whatever its destination. */\n"
        "    amaran_mesh_net_rx(rx->ctx.addr, rx->ctx.recv_dst, rx->ctl);\n\n")
j = src.index(CALL_ANCHOR)
src = src[:j] + CALL + src[j:]

if not os.path.exists(net + ".orig"):
    shutil.copy2(net, net + ".orig")
open(net, "w").write(src)
print("patched:", net)
