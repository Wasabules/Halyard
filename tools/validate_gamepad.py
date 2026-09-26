#!/usr/bin/env python3
"""validate_gamepad.py - a small window for validating gamepad detection.

Used to check, WITHOUT launching Shadow, that the detector really sees plug-ins,
unplugs, buttons and axes. We do not want to discover mid-capture that the
detector was silent.

The walkthrough is guided: each step validates itself as soon as it is
observee. Rien a taper.

    tools/validate_gamepad.py
"""
import glob, os, struct, time, tkinter as tk

IGNORES = ('shadow-uinput',)   # the virtual device Shadow creates itself

ETAPES = [
    ("depart",      "1. Etat de depart constate (automatique)"),
    ("branchee",    "2. BRANCHE la manette"),
    ("bouton",      "3. Press a BUTTON"),
    ("axis",        "4. Move a STICK or a trigger"),
    ("debranchee",  "5. DEBRANCHE-la"),
]

def nom_du_noeud(js):
    n = os.path.basename(js)
    for d in glob.glob('/sys/class/input/input*'):
        if os.path.exists(os.path.join(d, n)):
            try: return open(os.path.join(d, 'name')).read().strip()
            except OSError: return None
    return None

class App:
    def __init__(self, root):
        self.root = root
        root.title("Validation manette")
        root.geometry("560x420")
        self.etat = tk.Label(root, text="", font=("monospace", 11),
                             justify="left", anchor="w")
        self.etat.pack(fill="x", padx=14, pady=(14, 6))
        self.etapes_lbl = {}
        for cle, text_ in ETAPES:
            l = tk.Label(root, text="  ○  " + text_, font=("sans", 11),
                         anchor="w", fg="#777")
            l.pack(fill="x", padx=14, pady=2)
            self.etapes_lbl[cle] = l
        self.journal = tk.Text(root, height=8, font=("monospace", 9))
        self.journal.pack(fill="both", expand=True, padx=14, pady=(10, 14))

        self.faites = set()
        self.ouverts = {}
        self.connus = set()
        self.vue_une_fois = False
        self.premier_tour = True
        self.tick()

    def note(self, txt):
        self.journal.insert("end", f"{time.strftime('%H:%M:%S')}  {txt}\n")
        self.journal.see("end")

    def valide(self, cle):
        if cle in self.faites: return
        self.faites.add(cle)
        self.etapes_lbl[cle].config(text="  ●  " + dict(ETAPES)[cle], fg="#0a0")
        if len(self.faites) == len(ETAPES):
            self.note("=== detection VALIDATED on all five steps ===")

    def tick(self):
        presents = set(glob.glob('/dev/input/js*'))
        for js in sorted(presents - self.connus):
            nom = nom_du_noeud(js) or '?'
            self.connus.add(js)
            if any(x in nom.lower() for x in IGNORES):
                self.note(f"ignore (virtuel Shadow) : {nom}")
                continue
            try:
                fd = os.open(js, os.O_RDONLY | os.O_NONBLOCK)
            except OSError as e:
                self.note(f"ouverture refusee {js} : {e}")
                continue
            self.ouverts[js] = (fd, nom)
            self.vue_une_fois = True
            self.note(f"BRANCHEE  {nom}  ({js})")
            self.valide("branchee")
        for js in sorted(self.connus - presents):
            self.connus.discard(js)
            if js in self.ouverts:
                fd, nom = self.ouverts.pop(js)
                os.close(fd)
                self.note(f"DEBRANCHEE  {nom}")
                self.valide("debranchee")
        for js, (fd, nom) in list(self.ouverts.items()):
            while True:
                try: d = os.read(fd, 8)
                except (BlockingIOError, OSError): break
                if len(d) < 8: break
                _, valeur, typ, num = struct.unpack('<IhBB', d)
                if typ & 0x80: continue
                if typ & 0x01:
                    self.note(f"bouton #{num} = {valeur}")
                    self.valide("bouton")
                else:
                    self.note(f"axe #{num} = {valeur}")
                    self.valide("axe")

        reelles = [n for _, n in self.ouverts.values()]
        if self.premier_tour:
            # Step 1 asks for NOTHING: it observes the starting state. It used
            # to require an "unplug", which blocked when the
            # the gamepad was not plugged in to begin with.
            self.premier_tour = False
            self.note("etat de depart : " + (f"manette presente ({reelles[0]})"
                                             if reelles else "aucune manette"))
            if reelles:
                self.note("  -> for step 2: unplug it then plug it back in")
            self.valide("depart")
        self.etat.config(text=f"manettes vues : {reelles if reelles else 'aucune'}")
        self.root.after(50, self.tick)

if __name__ == '__main__':
    r = tk.Tk(); App(r); r.mainloop()
