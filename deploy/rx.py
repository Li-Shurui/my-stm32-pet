#!/usr/bin/env python3
# rx.py - robust serial file receiver for the ATK-MP157 board.
#
# Protocol (one line each, over the serial console):
#   data phase : "<seq> <md5_8> <base64chunk>"   seq is 1-based line number
#   "__END__"    : end of data phase, print a RESULT report, stay alive
#   fix phase    : "<seq> <md5_8> <base64chunk>" resent bad/missing lines
#   "__REPORT__" : print a RESULT report again (stays alive)
#   "__DONE__"   : assemble, write file, final report with md5
#
# Every line carries an 8-hex md5 of its base64 payload, so any dropped or
# corrupted character is detected. The host resends only the bad lines and
# may repeat the fix/report cycle until the receiver reports nothing bad.
#
# Reports: RESULT lines=N badlist=[..] missing=[..]  (full, untruncated)
# Final:   RESULT lines=N badlist=[] missing=[] md5=... then OK|MISMATCH
import sys, base64, hashlib

def decode_line(s):
    """Return (seq, raw_bytes) or None if the line is bad."""
    parts = s.split(" ", 2)
    if len(parts) != 3:
        return None
    try:
        seqn = int(parts[0])
    except Exception:
        return None
    chk, data = parts[1], parts[2]
    try:
        db = data.encode("ascii")
    except Exception:
        return None
    if hashlib.md5(db).hexdigest()[:8] != chk:
        return None
    if len(data) % 4 != 0:
        return None
    try:
        return (seqn, base64.b64decode(db, validate=True))
    except Exception:
        return None

def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: rx.py <path> [expect_md5]\n")
        return 2
    path = sys.argv[1]
    expect = sys.argv[2] if len(sys.argv) > 2 else ""

    chunks = {}
    bad_seqs = set()
    nlines = 0
    seen_max = 0
    mode = "data"

    def report():
        have = set(chunks.keys())
        still_bad = sorted(bad_seqs - have)
        missing = sorted(set(range(1, seen_max + 1)) - have)
        print("RESULT lines=%d badlist=%s missing=%s" % (nlines, still_bad, missing))
        sys.stdout.flush()
        return still_bad, missing

    while True:
        line = sys.stdin.readline()
        if not line:
            break
        s = line.strip()
        if not s:
            continue
        if s == "__END__":
            mode = "fix"
            report()
            continue
        if s == "__REPORT__":
            report()
            continue
        if s == "__DONE__":
            break
        if mode == "data":
            nlines += 1
            r = decode_line(s)
            if r is None:
                bad_seqs.add(nlines)
            else:
                chunks[r[0]] = r[1]
                if r[0] > seen_max:
                    seen_max = r[0]
        else:
            r = decode_line(s)
            if r is not None:
                chunks[r[0]] = r[1]
                if r[0] > seen_max:
                    seen_max = r[0]

    bad, missing = report()
    with open(path, "wb") as f:
        for s in sorted(chunks.keys()):
            f.write(chunks[s])
    h = hashlib.md5()
    with open(path, "rb") as f:
        while True:
            blk = f.read(65536)
            if not blk:
                break
            h.update(blk)
    got = h.hexdigest()
    print("FINAL lines=%d bad=%d missing=%d md5=%s" % (nlines, len(bad), len(missing), got))
    sys.stdout.flush()
    if expect and got != expect:
        print("MISMATCH")
        return 1
    print("OK")
    return 0

sys.exit(main())
