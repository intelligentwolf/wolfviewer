# [W94 VR CRASH 2026-11-05] Headset-like connection churn on the VR pipe's HTTPS port (plain HTTP to the TLS port,
# resets before/during the TLS handshake, page GETs, WebSocket upgrades with and without a hello, the same and a new
# device id). Found the g++ 11 coroutine teardown crash on the first headset that left. Used by coro_check.sh.
#   python3 -I coro_churn.py HOST PORT SECONDS
import socket, ssl, sys, time, random, threading, base64, os, struct
H, P, SECS = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
DEV = os.urandom(16).hex()
stop = time.time() + SECS; counts = {}
def rst(s):
    try: s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0)); s.close()
    except Exception: pass
def one(kind):
    s = socket.create_connection((H, P), timeout=5)
    if kind == 'plain':
        s.sendall(b'GET / HTTP/1.1\r\nHost: x\r\n\r\n'); s.recv(4096); s.close(); return
    if kind == 'rst_early':
        rst(s); return
    t = ssl.SSLContext.wrap_socket(ctx, s, server_hostname=H, do_handshake_on_connect=False)
    if kind == 'half_hs':
        t.setblocking(False)
        try: t.do_handshake()
        except Exception: pass
        time.sleep(random.random()*0.05); rst(s); return
    t.do_handshake()
    if kind == 'get':
        t.sendall(b'GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n'); t.recv(65536); rst(s); return
    key = base64.b64encode(os.urandom(16)).decode()
    t.sendall(('GET /ws?d=%s HTTP/1.1\r\nHost: %s:%d\r\nOrigin: https://%s:%d\r\nUser-Agent: Mozilla/5.0 (X11; Linux x86_64; Quest 3) OculusBrowser\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % (DEV if random.random()<0.7 else os.urandom(16).hex(), H, P, H, P, key)).encode())
    t.recv(4096)
    if kind == 'ws_hello':
        msg = b'{"t":"hello","w":1440,"h":1440}'; mask = os.urandom(4)
        t.sendall(bytes([0x81, 0x80 | len(msg)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(msg)))
        time.sleep(random.random()*0.3)
        try: t.recv(65536)
        except Exception: pass
    rst(s)
def worker():
    kinds = ['plain', 'rst_early', 'half_hs', 'get', 'ws', 'ws_hello']
    while time.time() < stop:
        k = random.choice(kinds)
        try: one(k); counts[k] = counts.get(k, 0) + 1
        except Exception as e: counts['err:' + type(e).__name__] = counts.get('err:' + type(e).__name__, 0) + 1
ts = [threading.Thread(target=worker) for _ in range(6)]
[t.start() for t in ts]; [t.join() for t in ts]
print(P, counts)
