#!/usr/bin/env python3
"""[WOLFLEAP 2026-10-24] Paul: "you could of course create some connection into the local viewer so you can control it -
it has my login credentials saved". A LEAP plugin (LLSD Event API Plugin, indra/llcommon/llleap.cpp): the viewer starts
it with  --leap "python3 -I <this file>"  and talks to it over stdin/stdout. It opens a Unix socket that only this user
can reach ($XDG_RUNTIME_DIR/wolfleap.sock, mode 0600 - no network port) so wolfctl can send requests to the viewer's
event APIs (LLAgent, LLViewerWindow, UI, LLChatBar, LLWindow, LLPanelLogin, ...) and read the replies.

Wire format (llleap.cpp):
  viewer -> plugin: "<length>:" + LLSDSerialize::serialize(packet, LLSD_BINARY), i.e. "<? LLSD/Binary ?>\\n" + binary LLSD
                    (llsdserialize.cpp LLSDBinaryFormatter::format_impl). The first packet is {pump: <our reply pump>,
                    data: {command: <LLLeapListener pump>, features: {...}}} (llleap.cpp:149-154).
  plugin -> viewer: "<length>:" + NOTATION LLSD of {pump, data} - required (llleap.cpp:331-335).
Requests carry data.reply = our pump and data.reqid; LLEventAPI::Response echoes reqid (lleventapi.h:100-108).
Socket protocol: one JSON object per line in, one JSON line out:
  {"pump": "LLAgent", "data": {"op": "getPosition"}, "timeout": 10, "noreply": false}
"""
import base64, datetime, itertools, json, os, socket, struct, sys, threading, uuid

SOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or os.path.expanduser("~/.cache"), "wolfleap.sock")
LOG = os.path.join(os.environ.get("XDG_RUNTIME_DIR") or os.path.expanduser("~/.cache"), "wolfleap.log")
LOG_MAX = 1 << 20   # Paul: no eternal logs - the log is cut back to empty past 1 MB

_out_lock = threading.Lock()
_pending = {}        # reqid -> [threading.Event, reply]
_pending_lock = threading.Lock()
_state = {"reply": None, "command": None, "features": None}
_ready = threading.Event()
_reqids = itertools.count(1)


def log(msg):
    try:
        if os.path.exists(LOG) and os.path.getsize(LOG) > LOG_MAX:
            open(LOG, "w").close()
        with open(LOG, "a") as f:
            f.write(datetime.datetime.now().strftime("%H:%M:%S ") + msg + "\n")
    except OSError:
        pass


# ---- binary LLSD (llsdserialize.cpp LLSDBinaryFormatter::format_impl) ----
def _parse_bin(b, i):
    t = b[i:i + 1]
    i += 1
    if t == b"!":
        return None, i
    if t == b"1":
        return True, i
    if t == b"0":
        return False, i
    if t == b"i":
        return struct.unpack(">i", b[i:i + 4])[0], i + 4
    if t == b"r":
        return struct.unpack(">d", b[i:i + 8])[0], i + 8              # ll_htond: network order
    if t == b"d":
        return {"__date__": struct.unpack("<d", b[i:i + 8])[0]}, i + 8   # written raw (host order, x86 little-endian)
    if t == b"u":
        return str(uuid.UUID(bytes=bytes(b[i:i + 16]))), i + 16
    if t in (b"s", b"l", b"b", b"k"):
        n = struct.unpack(">I", b[i:i + 4])[0]
        raw = bytes(b[i + 4:i + 4 + n])
        i += 4 + n
        if t == b"b":
            return {"__binary__": base64.b64encode(raw).decode()}, i
        return raw.decode("utf-8", "replace"), i
    if t == b"[":
        n = struct.unpack(">I", b[i:i + 4])[0]
        i += 4
        out = []
        for _ in range(n):
            v, i = _parse_bin(b, i)
            out.append(v)
        if b[i:i + 1] != b"]":
            raise ValueError("array not closed at %d" % i)
        return out, i + 1
    if t == b"{":
        n = struct.unpack(">I", b[i:i + 4])[0]
        i += 4
        out = {}
        for _ in range(n):
            if b[i:i + 1] != b"k":
                raise ValueError("map key expected at %d" % i)
            k, i = _parse_bin(b, i)
            v, i = _parse_bin(b, i)
            out[k] = v
        if b[i:i + 1] != b"}":
            raise ValueError("map not closed at %d" % i)
        return out, i + 1
    raise ValueError("unknown LLSD binary type %r at %d" % (t, i - 1))


def parse_packet(body):
    hdr = b"<? LLSD/Binary ?>\n"
    if body.startswith(hdr):
        body = body[len(hdr):]
    v, _ = _parse_bin(body, 0)
    return v


# ---- notation LLSD out ----
# LLLeap REQUIRES notation from the plugin: llleap.cpp:331-335 (`#if 1` - "specifically require notation LLSD from child";
# the SL-18330 any-format path is the disabled `#else`). Tokens per LLSDNotationParser::doParse (llsdserialize.cpp:549-700):
# ! undef, 1/0 booleans, i<int>, r<real>, u<uuid>, '<string>' with backslash escapes (deserialize_string_delim),
# [a,b] arrays, {'key':value} maps. (XML, even with "<? LLSD/XML ?>", is "unparseable LLSD data" - first live runs 10-24.)
def _q(s):
    return "'" + s.replace("\\", "\\\\").replace("'", "\\'").replace("\n", "\\n").replace("\r", "\\r") + "'"


def _nota(v):
    if v is None:
        return "!"
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, int):
        return "i%d" % v
    if isinstance(v, float):
        return "r%r" % v
    if isinstance(v, str):
        return _q(v)
    if isinstance(v, (list, tuple)):
        return "[" + ",".join(_nota(x) for x in v) + "]"
    if isinstance(v, dict):
        if set(v) == {"__uuid__"}:
            return "u" + str(uuid.UUID(v["__uuid__"]))
        return "{" + ",".join(_q(str(k)) + ":" + _nota(x) for k, x in v.items()) + "}"
    raise TypeError("cannot send %r" % (v,))


def send(pump, data):
    body = _nota({"pump": pump, "data": data}).encode()
    with _out_lock:
        sys.stdout.buffer.write(b"%d:" % len(body) + body)
        sys.stdout.buffer.flush()


# ---- viewer -> plugin ----
def reader():
    inp = sys.stdin.buffer
    while True:
        n = b""
        while True:
            c = inp.read(1)
            if not c:
                log("viewer closed stdin; exiting")
                try:
                    os.unlink(SOCK)
                except OSError:
                    pass
                os._exit(0)
            if c == b":":
                break
            n += c
        body = inp.read(int(n))
        try:
            pkt = parse_packet(body)
        except Exception as e:   # one bad packet must not end the bridge
            log("parse error: %s" % e)
            continue
        data = pkt.get("data") if isinstance(pkt, dict) else None
        if not _ready.is_set():
            _state["reply"] = pkt.get("pump")
            _state["command"] = (data or {}).get("command")
            _state["features"] = (data or {}).get("features")
            log("connected: reply pump %s, command pump %s" % (_state["reply"], _state["command"]))
            _ready.set()
            continue
        rid = data.get("reqid") if isinstance(data, dict) else None
        with _pending_lock:
            slot = _pending.get(rid)
        if slot:
            slot[1] = data
            slot[0].set()
        else:
            log("unsolicited: %s" % json.dumps(data)[:300])


# ---- socket: wolfctl -> viewer ----
def handle(conn):
    with conn:
        f = conn.makefile("rwb")
        line = f.readline()
        try:
            req = json.loads(line)
            if not _ready.wait(30):
                raise RuntimeError("the viewer has not connected to the bridge")
            pump = req.get("pump") or _state["command"]
            data = dict(req.get("data") or {})
            if req.get("noreply"):
                send(pump, data)
                out = {"ok": True, "sent": True}
            else:
                rid = next(_reqids)
                data["reply"] = _state["reply"]
                data["reqid"] = rid
                ev = threading.Event()
                with _pending_lock:
                    _pending[rid] = [ev, None]
                send(pump, data)
                got = ev.wait(float(req.get("timeout", 15)))
                with _pending_lock:
                    slot = _pending.pop(rid, None)
                out = {"ok": True, "reply": slot[1]} if got and slot else {"ok": False, "error": "no reply in time"}
        except Exception as e:
            out = {"ok": False, "error": str(e)}
        f.write((json.dumps(out) + "\n").encode())
        f.flush()


def server():
    try:
        os.unlink(SOCK)
    except OSError:
        pass
    old = os.umask(0o177)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(SOCK)
    os.umask(old)
    os.chmod(SOCK, 0o600)
    s.listen(8)
    log("listening on %s" % SOCK)
    while True:
        c, _ = s.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()


if __name__ == "__main__":
    threading.Thread(target=server, daemon=True).start()
    reader()
