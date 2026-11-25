#!/usr/bin/env python3
"""check_inventory.py : [HUD KIT 2026-11-25] every Wolf HUD's controls, checked against the source and the rig.

Paul, 25 Nov: "be careful you don't lose functionality or buttons when changing the look". Each HUD has an
inventory (inventory/<hud>.json) listing every button, key, toggle, gauge, readout, tooltip, hotkey, LEAP verb and
state, with the action it takes. This script proves a reskin kept all of them:

  static (always):   every control's "source" strings are still in the HUD's source files (whitespace-normalised),
                     and every hit id ("id": "H_...") is still both declared and used (two or more mentions).
  rig    (--log):    a WolfViewer.log from a LEAP rig run that called the HUD's "hits" verb. Every logged press
                     target ("WolfHudKit hit <hud> <id> gl x,y size w,h tip ...") must be an inventoried id, and
                     every one that is a button (kind != "tip") must be at least --min px (44) on both sides AT FULL
                     SIZE (the logged size over the zoom logHits reports; the resident may drag a HUD smaller), unless
                     its inventory entry carries "small_ok" (an agreed exception, with the reason).

Run from the repository root:
  python3 -I scripts/wolf/hudkit/check_inventory.py                 # all HUDs, static
  python3 -I scripts/wolf/hudkit/check_inventory.py traincab        # one HUD
  python3 -I scripts/wolf/hudkit/check_inventory.py traincab --log prof/logs/WolfViewer.log
Exit 0 = everything is there; 1 = something is missing (each named).
"""
import json, os, re, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
INV = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'inventory')


def norm(s):
    return re.sub(r'\s+', ' ', s).strip()


def load(hud):
    with open(os.path.join(INV, hud + '.json'), encoding='utf-8') as f:
        return json.load(f)


def static_check(inv):
    errs = []
    text = ''
    for rel in inv['files']:
        p = os.path.join(ROOT, rel)
        if not os.path.exists(p):
            errs.append('%s: source file missing: %s' % (inv['hud'], rel))
            continue
        with open(p, encoding='utf-8', errors='replace') as f:
            text += '\n' + f.read()
    flat = norm(text)
    n = 0
    for c in inv['controls']:
        n += 1
        for s in c.get('source', []):
            if norm(s) not in flat:
                errs.append('%s: %s (%s) lost its source line: %s' % (inv['hud'], c.get('id') or c.get('name'), c['kind'], s))
        cid = c.get('id', '')
        if cid.startswith('H_') and len(re.findall(r'\b%s\b' % re.escape(cid), text)) < 2:
            errs.append('%s: hit id %s is no longer used' % (inv['hud'], cid))
    return n, errs


LOGRX = re.compile(r'WolfHudKit hit (\S+) (\S+) gl (-?\d+),(-?\d+) size (\d+),(\d+) tip ?(.*)$')
# the header logHits writes before its targets: "hits <hud>: <n> (display scale <s>, zoom <k>)"
ZOOMRX = re.compile(r'hits (\S+): \d+ \(display scale [-\d.e]+, zoom ([-\d.e]+)\)')


def rig_check(inv, log, min_px):
    # The 44 px rule is at full size (zoom 1.0; the resident may drag a HUD smaller, wolfhudkit.h): each logged size is
    # divided by the zoom the HUD was drawn at. A control with "small_ok" (an agreed exception, with its reason) is noted,
    # not failed.
    errs = []
    # a control is known by its "id", or by its "name" when that is a hit id (the flight deck's LSK L2-L5); a ranged id the
    # HUD logs as "<first>+n" (the flight deck's seats and engine keys) is its range's first id
    known = {}
    for c in inv['controls']:
        for k in (c.get('id'), c.get('name')):
            if k and k.startswith('H_'):
                known.setdefault(k, c)
    seen = 0
    zoom = 1.0
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            z = ZOOMRX.search(line)
            if z and z.group(1) == inv['hud']:
                zoom = float(z.group(2)) if float(z.group(2)) > 0 else 1.0
                continue
            m = LOGRX.search(line)
            if not m or m.group(1) != inv['hud']:
                continue
            seen += 1
            hid = m.group(2).split('+')[0]
            w, h = int(m.group(5)) / zoom, int(m.group(6)) / zoom
            c = known.get(hid)
            if c is None:
                errs.append('%s: the rig logged %s, which is not in the inventory' % (inv['hud'], hid))
                continue
            if c['kind'] != 'tip' and (w < min_px - 0.5 or h < min_px - 0.5):
                if c.get('small_ok'):
                    print('note: %s %s is %.0f x %.0f px at full size (agreed: %s)' % (inv['hud'], hid, w, h, c['small_ok']))
                else:
                    errs.append('%s: %s is %.0f x %.0f px at full size on the rig (at least %d)' % (inv['hud'], hid, w, h, min_px))
    if seen == 0:
        errs.append('%s: the log has no "WolfHudKit hit %s" lines (was the hits verb called while it was shown?)' % (inv['hud'], inv['hud']))
    return seen, errs


def main(argv):
    args = [a for a in argv if not a.startswith('--')]
    log = None
    min_px = 44
    for i, a in enumerate(argv):
        if a == '--log':
            log = argv[i + 1]
            args.remove(log)
        if a == '--min':
            min_px = int(argv[i + 1])
            args.remove(argv[i + 1])
    huds = args or sorted(f[:-5] for f in os.listdir(INV) if f.endswith('.json'))
    bad = []
    for hud in huds:
        inv = load(hud)
        n, errs = static_check(inv)
        line = '%-12s %3d controls' % (hud, n)
        if log:
            seen, rerrs = rig_check(inv, log, min_px)
            errs += rerrs
            line += ', %d rig targets' % seen
        print(line + (' OK' if not errs else ' FAILED'))
        bad += errs
    for e in bad:
        print('  ' + e)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
