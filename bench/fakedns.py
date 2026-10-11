#!/usr/bin/env python3
"""fakedns: a small authoritative-style DNS server over UDP for the lab.

    python3 -I bench/fakedns.py ZONEFILE PORT [BIND]      (BIND: 0.0.0.0)

It answers A and AAAA questions for the names in ZONEFILE, which it reads
again whenever the file changes (mtime, size or inode, checked per query), so
a scenario can change the "DNS" under a running client by rewriting the file
(write a temporary file and mv it over, so that a reader never sees half of
it). Python 3 standard library only. Used by bench/lab.d/names.sh.

Zone lines, "#" starts a comment, names are not case sensitive:

    vps.lab A 198.51.100.7 60       an address and its TTL (default 60 s)
    vps.lab AAAA 2001:db8::7 60     (several lines per name: several answers)
    vps.lab NXDOMAIN                the name does not exist
    vps.lab SERVFAIL                the server fails
    vps.lab DROP                    no answer at all (a dead resolver)

A special line wins over the records of its name (DROP, then SERVFAIL, then
NXDOMAIN). A name that is not in the zone is NXDOMAIN. A name in the zone
without a record of the type asked is NODATA (NOERROR, no answers). A class
other than IN is REFUSED.

The answer copies the question and sets QR, AA, RD (as asked) and RA; each
answer record names its owner with the compression pointer 0xC00C. No EDNS:
an OPT record in the query is ignored. Garbage never stops the server: a
datagram that is no query is dropped, a query with a broken question gets
FORMERR.

With FAKEDNS_LOG=FILE, each question is appended to FILE as one line,
"time qname qtype client" (epoch seconds, even when the answer is DROP), so
that a scenario can count the lookups.
"""
import os
import socket
import struct
import sys
import time

TYPES = {"A": 1, "AAAA": 28}
NAMES = {v: k for k, v in TYPES.items()}
ANY = 255
NOERROR, FORMERR, SERVFAIL, NXDOMAIN, NOTIMP, REFUSED = 0, 1, 2, 3, 4, 5
SPECIAL = ("DROP", "SERVFAIL", "NXDOMAIN")  # in precedence order


def norm(name):
    return name.strip().rstrip(".").lower()


def load(path):
    """The zone: {name: {"special": str|None, "A": [(rdata, ttl)], "AAAA": [...]}}."""
    zone = {}
    with open(path, errors="replace") as f:
        for n, line in enumerate(f, 1):
            w = line.split("#", 1)[0].split()
            if not w:
                continue
            try:
                ent = zone.setdefault(norm(w[0]), {"special": None, "A": [], "AAAA": []})
                kind = w[1].upper()
                if kind in SPECIAL:
                    if ent["special"] is None or SPECIAL.index(kind) < SPECIAL.index(ent["special"]):
                        ent["special"] = kind
                elif kind in TYPES:
                    fam = socket.AF_INET if kind == "A" else socket.AF_INET6
                    ent[kind].append((socket.inet_pton(fam, w[2]), int(w[3]) if len(w) > 3 else 60))
                else:
                    raise ValueError("unknown record " + kind)
            except (IndexError, ValueError, OSError) as e:
                print(f"fakedns: {path}:{n}: ignored: {e}", file=sys.stderr)
    return zone


def question(msg):
    """(qname, qtype, qclass, end) of the one question of a query, or None."""
    labels, off = [], 12
    while True:
        if off >= len(msg):
            return None
        n = msg[off]
        if n == 0:
            off += 1
            break
        if n > 63 or off + 1 + n > len(msg):  # also compression pointers (0xC0): none in a question
            return None
        labels.append(msg[off + 1 : off + 1 + n].decode("ascii", "replace"))
        off += 1 + n
    if off + 4 > len(msg) or sum(map(len, labels)) + len(labels) > 255:
        return None
    qtype, qclass = struct.unpack_from("!HH", msg, off)
    return norm(".".join(labels)), qtype, qclass, off + 4


def reply(query, rcode, qend=12, answers=()):
    """The response to query: its header with QR AA RA, the question, the answers."""
    flags = 0x8000 | (query[2] & 0x79) << 8 | 0x0400 | 0x0080 | rcode  # QR, opcode+RD as asked, AA, RA
    head = struct.pack("!HHHHHH", struct.unpack_from("!H", query)[0], flags, 1 if qend > 12 else 0, len(answers), 0, 0)
    body = b"".join(struct.pack("!HHHIH", 0xC00C, t, 1, ttl, len(rd)) + rd for t, rd, ttl in answers)
    return head + query[12:qend] + body


def answer(zone, query):
    """(response, qname, qtype) for a query datagram; response None: send nothing."""
    if len(query) < 12 or query[2] & 0x80:  # shorter than a header, or a response
        return None, None, None
    if query[2] >> 3 & 15 or struct.unpack_from("!H", query, 4)[0] != 1:
        return reply(query, NOTIMP), None, None  # not a standard query with one question
    q = question(query)
    if not q:
        return reply(query, FORMERR), None, None
    name, qtype, qclass, qend = q
    shown = NAMES.get(qtype, f"TYPE{qtype}")
    if qclass != 1:
        return reply(query, REFUSED, qend), name, shown
    ent = zone.get(name)
    if ent is None or ent["special"] == "NXDOMAIN":
        return reply(query, NXDOMAIN, qend), name, shown
    if ent["special"] == "DROP":
        return None, name, shown
    if ent["special"] == "SERVFAIL":
        return reply(query, SERVFAIL, qend), name, shown
    kinds = ("A", "AAAA") if qtype == ANY else [NAMES[qtype]] if qtype in NAMES else []
    return reply(query, NOERROR, qend, [(TYPES[k], rd, ttl) for k in kinds for rd, ttl in ent[k]]), name, shown


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__.split("\n\n")[0] + "\nusage: fakedns.py ZONEFILE PORT [BIND]")
    path, port, bind = sys.argv[1], int(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else "0.0.0.0"
    sock = socket.socket(socket.AF_INET6 if ":" in bind else socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((bind, port))
    log = open(os.environ["FAKEDNS_LOG"], "a", buffering=1) if os.environ.get("FAKEDNS_LOG") else None
    zone, seen = {}, None
    while True:
        try:
            query, client = sock.recvfrom(4096)
            try:  # the zone, again when the file changed
                st = os.stat(path)
                if (st.st_mtime_ns, st.st_size, st.st_ino) != seen:
                    zone, seen = load(path), (st.st_mtime_ns, st.st_size, st.st_ino)
            except OSError as e:  # missing for a moment: keep the last zone
                print(f"fakedns: {path}: {e}", file=sys.stderr)
            resp, name, qtype = answer(zone, query)
            if log and name:
                log.write(f"{time.time():.3f} {name} {qtype} {client[0]}:{client[1]}\n")
            if resp:
                sock.sendto(resp, client)
        except OSError:
            pass  # a send that failed, a signal
        except Exception as e:  # never stop for a message
            print(f"fakedns: {type(e).__name__}: {e}", file=sys.stderr)


if __name__ == "__main__":
    main()
