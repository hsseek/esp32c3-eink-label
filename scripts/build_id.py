"""Stamps each build with a UTC timestamp and the git commit it came from.

Without this there is no way to tell which firmware a device is running, which
matters once updates arrive over the air rather than over USB: an upload that
silently failed looks exactly like one that worked.

The hash is HEAD at build time, so a binary committed afterwards names the
commit its *source* came from. A '+' suffix means the tree had uncommitted
changes and the hash alone does not describe what was built.
"""
Import("env")
import subprocess
from datetime import datetime, timezone


def _git(*args):
    try:
        return subprocess.check_output(("git",) + args, stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return ""


stamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%MZ")
head = _git("rev-parse", "--short=7", "HEAD")
if head:
    dirty = subprocess.call(("git", "diff", "--quiet", "HEAD"), stderr=subprocess.DEVNULL) != 0
    build_id = "%s %s%s" % (stamp, head, "+" if dirty else "")
else:
    build_id = stamp                      # built outside a git checkout

print("build id: %s" % build_id)
env.Append(CPPDEFINES=[("BUILD_ID", env.StringifyMacro(build_id))])
