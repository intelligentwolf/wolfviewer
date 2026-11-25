#!/usr/bin/env python3
"""check_inventory.py : [HUD KIT 2026-11-25] every Wolf HUD's controls, checked against the source and the rig.

Paul, 25 Nov: "be careful you don't lose functionality or buttons when changing the look". Each HUD has an
inventory (inventory/<hud>.json) listing every button, key, toggle, gauge, readout, tooltip, hotkey, LEAP verb and
state, with the action it takes. This script proves a reskin kept all of them:

  static (always):   every control's "source" strings are still in the HUD's source files (whitespace-normalised),
                     and every hit id ("id": "H_...") is still both declared and used (two or more mentions).
  rig    (--log):    a WolfViewer.log from a LEAP rig run that called the HUD's "hits" verb. Every logged press
                     target ("WolfHudKit hit <hud> <id> gl x,y size w,h tip ...") must be an inventoried id, and
                     every one that is a button (kind != "tip") must be at least --min px (44) on both sides.

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


def rig_check(inv, log, min_px):
    errs = []
    known = {c['id']: c for c in inv['controls'] if c.get('id')}
    seen = 0
    with open(log, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = LOGRX.search(line.rstrip('\n'))
            if not m or m.group(1) != inv['hud']:
                continue
            seen += 1
            hid, w, h = m.group(2), int(m.group(5)), int(m.group(6))
            c = known.get(hid)
            if c is None:
                errs.append('%s: the rig logged %s, which is not in the inventory' % (inv['hud'], hid))
                continue
            if c['kind'] != 'tip' and (w < min_px or h < min_px):
                errs.append('%s: %s is %d x %d px on the rig (at least %d)' % (inv['hud'], hid, w, h, min_px))
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
