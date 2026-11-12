# Capture real SPU states at the entry of lifted functions, for the offline
# benchmark (scripts/test/spu_loop_bench.py). Inserts a call to
# gt5p_spu_capture (src/gt5p_spu_native.c) at each; the game writes states
# only while GT5P_SPU_CAPTURE=<file> is set (see that function for the
# settings; each address keeps its own schedule and count).
#   .venv\Scripts\python scripts\diag\spu_capture_patch.py b01697100 7FE0 [7DC8 ...]
#   .venv\Scripts\python scripts\diag\spu_capture_patch.py --restore b01697100
# Rebuild after each. The state file is game-derived: keep it in scratch/.
# Run it after the other post-lift steps (spu_region.py last), and restore
# before running them again.
import os, re, sys

args = [a for a in sys.argv[1:] if a != "--restore"]
body = args[0]
p = os.path.join(os.path.dirname(__file__), "..", "..", "generated", "spu_lifted", body, "spu_recomp.c")
bak = p + ".cap_orig"
if "--restore" in sys.argv:
    if os.path.exists(bak):
        os.replace(bak, p)
        os.utime(p)   # newer than an object built from the patched copy
        print("restored", p)
    sys.exit(0)
addrs = [int(a, 16) for a in args[1:]]
src = open(bak if os.path.exists(bak) else p, encoding="latin-1", newline="").read()
if not os.path.exists(bak):
    open(bak, "w", encoding="latin-1", newline="").write(src)
out = src
for addr in addrs:
    head = re.compile(r"^void gt5p_%s_spu_func_%08X\(spu_context\* ctx\) \{\r?\n" % (body, addr), re.M)
    if len(head.findall(out)) != 1:
        sys.exit("function 0x%X not found once in %s" % (addr, p))
    out = head.sub(lambda m: m.group(0) + "        gt5p_spu_capture(ctx, 0x%Xu);\n" % addr, out)
# The declaration must follow the includes: put it after the last #include.
lines = out.split("\n")
last = max(i for i, l in enumerate(lines) if l.startswith("#include"))
lines.insert(last + 1, "void gt5p_spu_capture(spu_context* ctx, unsigned addr);")
open(p, "w", encoding="latin-1", newline="").write("\n".join(lines))
print("capture hooks at %s in %s" % (", ".join("0x%X" % a for a in addrs), p))
