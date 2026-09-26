#!/usr/bin/env python3
"""test_input_any.py - counts EVERY input event, per device.

A passive diagnostic: instead of asking for a precise click, we look at whether
the slightest event (a movement, a key, a button) arrives. If nothing arrives while
while the user moves the mouse or types, the reading is blocked (a compositor
holding the devices exclusively, for instance) - and that is not the
worth relaunching any captures.
"""
import glob, os, select, struct, sys, time, collections
FMT='llHHi'; SZ=struct.calcsize(FMT)
TYP={0:'SYN',1:'KEY',2:'REL',3:'ABS',4:'MSC'}
names={}
for b in open('/proc/bus/input/devices').read().split('\n\n'):
    nm=next((l[9:-1] for l in b.splitlines() if l.startswith('N:')),'?')
    for l in b.splitlines():
        if l.startswith('H:'):
            for w in l.split():
                if w.startswith('event'): names[f"/dev/input/{w}"]=nm
fds={}
for p in sorted(glob.glob('/dev/input/event*')):
    try: fds[os.open(p,os.O_RDONLY|os.O_NONBLOCK)]=p
    except OSError as e: print(f"  ouverture refusee {p}: {e}")
dur=float(sys.argv[1]) if len(sys.argv)>1 else 15.0
print(f"{len(fds)} peripheriques ouverts.")
print(f"MOVE THE MOUSE and PRESS a few keys for {dur:.0f}s "
      f"(no need to click)\n")
cnt=collections.Counter(); typ=collections.Counter()
end=time.time()+dur
while time.time()<end:
    r,_,_=select.select(list(fds),[],[],0.3)
    for fd in r:
        try: d=os.read(fd,SZ*256)
        except OSError: continue
        for o in range(0,len(d)-SZ+1,SZ):
            _,_,t,c,v=struct.unpack(FMT,d[o:o+SZ])
            cnt[fds[fd]]+=1; typ[TYP.get(t,str(t))]+=1
for fd in fds: os.close(fd)
tot=sum(cnt.values())
print(f"{tot} evenements lus au total\n")
for p,c in cnt.most_common(8):
    print(f"   {c:>6}  {p}  « {names.get(p,'?')} »")
print(f"\npar type : {dict(typ)}")
print("\n=> LECTURE FONCTIONNELLE" if tot else
      "\n=> READING IS BLOCKED: no event at all, while you were moving/typing")
