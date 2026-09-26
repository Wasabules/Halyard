# Porting to the PS Vita — the measured ground

Written 2026-09-12, after the restructuring isolated the core. Nothing here is
an estimate unless it says so: every number below was produced by compiling or
running something on this machine.

## Where it stands

**`halyard.vpk` builds** — 9.4 MB, from `cmake -DPLATFORM_PSV=ON -DUSE_GXM=ON`.
All 56 core files compile, the whole client links, and `vita-elf-create`
packages it.

### What the first install taught, before it even ran

Two things were wrong that no amount of compiling could have shown, and both
were the same defect as everywhere else - a `#else` that meant "Linux".

- **The data directory.** `SHADOW_DATA_DIR` fell through to
  `/tmp/halyard/`, which does not exist on a Vita. On a console that
  directory IS the interface: no `env.txt` (the only way to set a toggle where
  there is no shell), no `logsink.txt` (the only way to see a log at all), no
  `refresh_token`. The first `.vpk` could write nothing and report nothing.
  Now `ux0:data/halyard/`, and `main()` CREATES it before the journal
  opens - it used to be made only at pairing time, by `oauth.c`, which is far
  too late. The Switch got away with that because `switch-sync.sh` makes the
  directory; a fresh install has nobody to do it.
- **The network did not exist.** `shadow_sockets_init()` was
  `#ifdef __SWITCH__` with a `return 0` for everything else - true on Linux,
  where a socket call just works, and false here: `sceNetInit` must be called
  with a caller-owned memory pool before any socket exists. Borealis does not
  do it either (checked). Without it the client shows its interface and fails
  at the first HTTP request, with an error that says nothing about the cause.
  1 MB pool, `SHADOW_VITA_NET_POOL` to move it.

### 2026-09-13, on a real console: everything up to the decoder

The `.vpk` runs. Measured from the console's own log, in order:

```
[data] ux0:data/halyard/ : writable
network: SceNet up, 1048576 B pool
[S88] interface sounds: 14/14 loaded
sfx: SceAudioOut BGM opened - 48000 Hz, 2 ch, grain 512 frames (10.67 ms)
[connecting] use_native=1   port_base computed = 8000
[sse] connecting .../1/stream          (twice - the dual SSE)
[mode] portable -> 1280x720
[AUDC1] interface sounds suspended during the stream
h264: avcodec_find_decoder FAIL        <- and here it stops
```

The whole REST bootstrap returns 200 (capabilities, vm/start, vm/ip, auth_login,
proximus-credentials), the token now persists, the dual SSE opens, the port base
is computed, and the interface hands its audio output to the stream. Then:

**THE VITASDK'S libavcodec HAS NO H.264 DECODER.** 22 decoders are built in and
that is not one of them - h263, mpeg4, svq1, cinepak, and, usefully, FLAC and
Opus, so the AUDIO path has what it needs. Almost certainly a licensing choice
in the package.

So the picture needed two things that are separate:
- **a decoder** — DONE, `core/media/h264_decoder_vita.{c,h}`: SceAvcdec, the
  hardware. `SceAvcdecAu` takes the elementary-stream buffer with a PTS, which
  is exactly what `vid_reasm.c` already emits, so nothing upstream changed and
  the access unit is handed over BY POINTER — no copy on that path. Opened at a
  1920x1088 ceiling rather than at the stream's size (`sceAvcdecCreateDecoder`
  reserves for a maximum and smaller streams decode inside it, and this
  constructor never receives the negotiated resolution);
  `SHADOW_VITA_DEC_MAX_W/H` lowers it, `SHADOW_VITA_DEC_REFS` the DPB depth.
  **Never run on hardware.**
- **a renderer** — still none; see below. Decoding without one means the
  session runs, the counters move and `[L5]` measures, with nothing on screen.

**The stream has never rendered.** Two things are known missing rather than
suspected: there is **no video renderer** (the 104 GL calls of
`gl_video_renderer.cpp` are a desktop/EGL shader and the Vita renders through
GXM - the class exists, `init()` refuses, and the log says so), and the audio
backend **has never been heard**. Everything else - session, decode, input,
network, TLS - compiles and links, which is not the same as working.

```bash
export VITASDK=$HOME/vitasdk PATH="$VITASDK/bin:$PATH"
bash tools/bootstrap-libs.sh          # wolfSSL + jansson, built for the Vita
cmake -S . -B build_psv -G Ninja -DPLATFORM_PSV=ON -DUSE_GXM=ON \
      -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake --build build_psv               # -> build_psv/halyard.vpk
```

## The short version

**All 56 core files compile for `arm-vita-eabi`.** `tools/vita-survey.sh`
reproduces that and names any regression.

Forty-nine of them needed no change at all. The other two are the audio, which
was the one real architectural gap and is now closed: `core/media/audio.c`
selects its sink explicitly per target instead of assuming ALSA, and
`core/media/audio_out_vita.{c,h}` is a SceAudioOut backend behind the shared
ring. **It has never been heard** — see below. The cipher —
the thing worth doubting first on a 444 MHz Cortex-A9 — is not the problem.

## What is already true

| | |
|---|---|
| Toolchain | `arm-vita-eabi-gcc 15.2.0`, installed under `$VITASDK` (394 MB) |
| SDK | 488 `Sce*` stub libraries, including `SceAvcodec` (hardware H.264) and `SceGxm` (GPU) |
| Portlibs | 52, including curl, jansson, opus, FLAC, libavcodec, mbedTLS, vitaGL, SDL2 |
| wolfSSL | **builds for the Vita** — 1.3 MB static, with DTLS, ChaCha20-Poly1305, TLS 1.3 and SNI |
| The UI | Borealis already targets PSV: the CMake has `PLATFORM_PSV`, `vita_create_vpk`, VitaShark, and the LiveArea assets are in `clients/borealis/psv/` |
| The decoder's shape | `SceAvcdecAu` takes an elementary-stream buffer with PTS/DTS — which is exactly what `vid_reasm.c` already emits (Annex-B access units per PTS) |

wolfSSL needed **the same flags the Switch needs** and failed the same way
without them: no `NO_WRITEV` and it pulls `<sys/uio.h>`, which does not exist
here. The project notes had already written that down for the Switch; it transferred
without a change.

## What it took, and what each fix says about the codebase

Every blocker was the same shape: a condition that named the platforms it was
NOT, which held while there were three targets and broke on the fourth.

| Was | Became |
|---|---|
| `audio.c`: `#else` = ALSA | each target names its sink; an unknown one gets none and still builds |
| `sfx.cpp`: `#else` = ALSA | the same |
| `wolfssl_rand_switch.c`: `#else` = `sys/random.h` | each names its RNG; an unknown one **fails to compile on purpose** - a silent zero-filled seed is the worst possible degradation |
| `ctrl_gamepad.c`: `!__SWITCH__ && !_WIN32` = evdev | `__linux__ \|\| __FreeBSD__`, and a handheld console IS a gamepad |
| `qr_helper.c`: `#ifdef __SWITCH__` | `SHADOW_HAVE_QRENCODE`, a property of the toolchain rather than the console |
| five files: `#ifndef __SWITCH__` = has GLFW/glad | `clients/borealis/gl_compat.h`, asked once |
| `connecting_activity.cpp` + `shadow_input.c`: two private copies of the DC platform list | `SHADOW_HAVE_LEGACY_DC`, declared by the CMake that actually decides |
| `win_stubs.c` | `legacy_dc_stubs.c` - and it turned out **Switch never defined `sctp_send_msg` at all**: its link only worked because the caller was unreachable and got discarded. The Vita, whose link keeps relocations for `vita_create_self`, found it at once |

Three things had to be built or fixed outside the code:

- **wolfSSL**, out of band like every target, with the flags the Switch already
  needed (`NO_WRITEV`, or its headers pull `<sys/uio.h>`) and **without
  `opensslextra`** - the SDK's curl was built against OpenSSL, and wolfSSL's
  compat layer exports the same `SSL_*`/`BIO_*` names.
- **libcurl, rebuilt on Mbed TLS** (2026-09-26). The SDK's is built on OpenSSL
  1.0.2 and linked statically, and the OpenSSL 1.x licence cannot be combined
  with GPL code: the `.vpk` could not be redistributed. `tools/build-libs.sh
  vita curl` builds the same curl 8.17.0 with the vitasdk's own options and the
  SDK's Mbed TLS 3.6.5; the ELF then holds no OpenSSL code at all (checked by
  symbol and by string).
- **FFmpeg, FLAC decoder only** (2026-09-26). Video is SceAvcdec; libavcodec
  only decodes FLAC audio. The vitasdk's FFmpeg brought LAME and mpg123 (LGPL)
  at versions vdpm does not pin, so a release could not name their source.
  `tools/build-libs.sh vita ffmpeg` builds the tarball the Switch pins, with
  the vitasdk recipe's target flags: the `.vpk` went from 6.5 to 5.1 MB.
- **jansson**, rebuilt without PIC. The SDK packages it with `-fPIC`, and
  `vita-elf-create` refuses the GOT-relative relocations that produces
  (`Invalid relocation type 25`). It was the last thing between a linked ELF and
  a `.vpk`, and it took finding 19 relocations in one archive to see it.
- **One line of Borealis**, guarded: `SCE_GXM_ERROR_INVALID_AUXILIARY_SURFACE`
  is not in this vitasdk, and the whole table is error STRINGS. In
  `patches/borealis/local-fixes.patch`.

## The cipher question, settled enough to stop asking it

Measured with `tools/client/bench_chacha.c` on the real `encryption.c`, Linux
desktop x86-64: **327 960 chunks/s, 3.05 µs per 1241 B chunk.** A 720p stream
needs 806 chunks/s — 0.2 % of that CPU; the 25 Mb/s console ceiling needs
2 518, or 0.8 %.

That is a desktop number and the Vita is not this CPU — **nobody has run the
bench on the hardware, and until someone does this is an extrapolation.** But
the margin is the argument: even a hundredfold penalty, far worse than scalar
ChaCha20 on ARMv7 has any business being, leaves 720p at about 20 % of one
core. Cross-compile the bench and run it once; it takes no dependency beyond
the cipher precisely so that it can be.

## What to ask next, in this order

1. **`SceAvcdec` throughput and latency at 960×544.** The screen is 960×544, so
   720p decoded and downscaled is the sane target, not 1080p. This is now the
   least-known number in the chain.
2. **Memory.** 512 MB, and `SHADOW_DEC_QUEUE` defaults to 16 frames — CONC-1
   measured that depth as the one that stops drops on desktop. At 1.38 MB a
   frame that is 22 MB of queue alone, before the decoder's own surfaces.
3. **~~The audio backend~~ — written, and the part that is still open.** The
   selection ladder now names every target, `blocking_play_loop` serves ALSA and
   SceAudioOut alike (their writes both block; WASAPI keeps its own loop, driven
   by a period event), and the Vita's grain constraint — `sceAudioOutOpenPort`
   demands a multiple of 64, the play thread writes 480 — is absorbed inside the
   sink rather than pushed onto the shared chunk.
   **What is NOT settled: nobody has heard it.** The 512-frame grain (10.67 ms)
   is a reasoned choice, not a measured one, and `grain_us` is reported in the
   open log precisely so the first person with hardware can judge it. Expect to
   move that constant.
4. **The network stack.** `SceNet` is BSD-shaped, and the 48 files that already
   compile include every socket path — but compiling is not connecting.

## How to reproduce the survey

```bash
export VITASDK=$HOME/vitasdk PATH="$VITASDK/bin:$PATH"
V=$VITASDK/arm-vita-eabi
for f in core/*/*.c; do
  arm-vita-eabi-gcc -fsyntax-only -O2 "$f" \
    -I. -I$V/include -I$V/include/opus \
    -Ithird_party/wolfssl -Ithird_party/wolfssl/build_vita \
    -DNO_WRITEV -DNO_WOLFSSL_DIR || echo "KO $f"
done
```

wolfSSL for the Vita is built out of band, like every other target
(`CMakeLists.txt` does not drive it):

```bash
cmake -S third_party/wolfssl -B third_party/wolfssl/build_vita -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
  -DCMAKE_C_FLAGS="-DNO_WRITEV -DNO_WOLFSSL_DIR -D_POSIX_THREADS=1 -D_REENTRANT" \
  -DBUILD_SHARED_LIBS=OFF -DWOLFSSL_EXAMPLES=OFF -DWOLFSSL_CRYPT_TESTS=OFF \
  -DWOLFSSL_DTLS=yes -DWOLFSSL_CHACHA=yes -DWOLFSSL_POLY1305=yes \
  -DWOLFSSL_OPENSSLEXTRA=no -DWOLFSSL_TLS13=yes -DWOLFSSL_SNI=yes
cmake --build third_party/wolfssl/build_vita
```

**Deux corrections de cette recette, 2026-09-13, et chacune avait déjà coûté.**

`OPENSSLEXTRA` disait `yes` ici alors que le paragraphe six lignes plus haut dit
« **sans `opensslextra`** » et que le `CMakeCache.txt` réel porte `no`. Le bloc
était périmé : le copier-coller reproduisait la collision de symboles avec le
curl OpenSSL du SDK.

Et il manquait la **source d'entropie**. Les deux options ajoutées au
`CMAKE_C_FLAGS` ci-dessus ne sont pas facultatives :

```
-DCUSTOM_RAND_GENERATE_SEED=switch_rand_seed
-include $PWD/patches/wolfssl/vita_seed_decl.h
```

Sans elles, wolfSSL garde son propre `wc_GenerateSeed`, qui lit `/dev/urandom` —
la Vita n'en a pas. Le `-include` fournit le prototype que `random.c` demande
explicitement à l'intégrateur et ne déclare pas : sous GCC 15, dont le défaut
est C23, une déclaration implicite est une **erreur**.

La vérification qui prouve que c'est bon ne se lit pas dans le code :

```bash
strings third_party/wolfssl/build_vita/libwolfssl.a | grep -c dev/urandom   # doit dire 0
arm-vita-eabi-nm third_party/wolfssl/build_vita/libwolfssl.a | grep Seed
#   U switch_rand_seed        <- wc_GenerateSeed appelle bien la notre
#   T wc_GenerateSeed
```

L'archive Switch donne 0 depuis toujours ; c'est la comparaison entre les deux
qui a désigné le défaut.

## 2026-09-13 — le décodeur refusait tout, et le panneau promettait cinq choses

**`sceVideodecInitLibrary` rendait `0x80620802`** sur chaque session, chaque
tentative : `SCE_VIDEODEC_ERROR_INVALID_PARAM`. Le code d'erreur importe — ce
n'est **pas** `OUT_OF_MEMORY` (`0x80620803`), donc il ne s'agissait pas de
quelques mégaoctets mais d'un plafond de géométrie dur. Le décodeur ouvrait à
1920×1088 « parce qu'une réservation plus large supprime toute une classe de
pannes » ; le raisonnement était écrit dans le commentaire, et il était faux.

Corrigé en **mesurant au lieu de parier** : `sceVideodecInitLibrary` est un
oui/non gratuit pris avant la première image, donc une échelle descendante
(1920×1088/3 refs → 1280×720/4, /3, /2 → 960×544/3 → 640×368/2) coûte quelques
appels ratés une fois par session et **journalise la configuration accordée**.
La première exécution console répond donc définitivement à « quel est le vrai
plafond ? ». `SHADOW_VITA_DEC_MAX_W/H/REFS` **imposent** encore une géométrie et
court-circuitent l'échelle, conformément à la règle des bascules.

Les quatorze codes `SceVideodec` sont désormais nommés dans le journal : un
`0x80620802` nu coûtait un aller-retour dans les en-têtes du SDK à chaque fois.

**Le panneau de paramètres offrait cinq lignes sans rien derrière** sur Vita —
choix matériel/logiciel de décodage, haptique, pointeur de démonstration,
coupure à la perte de focus, verrou au réveil. Le dépôt avait déjà écrit deux
fois le raisonnement (`SET_HW_AUDIO`, `SET_POINTER`) : « proposer un réglage
sans rien derrière jette le doute sur tous les autres ». Sur Vita cinq le
faisaient d'un coup.

`clients/borealis/device_caps.h` **nomme les capacités** au lieu de soustraire
des plateformes. C'est la leçon centrale de ce port : `SET_POINTER` était gardé
par `#if !defined(__SWITCH__)`, qui voulait dire « bureau » — vrai tant qu'il
n'y avait que deux cibles, et qui a silencieusement rallumé la ligne le jour où
la Vita est arrivée, sur une machine sans souris.

Le verrou au réveil a **sa propre** macro bien qu'il vienne du même crochet
applet que le focus : le bureau n'a pas l'événement mais a `SHADOW_DIAG_RELOCK_S`
(AF2), qui refuse d'agir si ce réglage est éteint. Les fusionner aurait
discrètement désarmé une campagne existante en rangeant un menu.

**Le testeur de manette dessine une Vita.** Il existe pour comparer ce que la
console LIT à ce qu'on ENVOIE ; dessiner une Switch ruinait la comparaison au
premier coup d'œil. Corps d'un seul tenant sans poignées, croix au-dessus du
stick gauche, symboles au-dessus du stick droit, L et R seuls, SELECT/START en
bas à droite. Les quatre symboles sont des **tracés vectoriels**, pas des
glyphes UTF-8 : la police du thème pourrait ne pas couvrir U+25B3 et dessiner un
tofu sur le seul écran dont le métier est d'être cru.

ZL, ZR et les deux clics de stick ne sont **pas dessinés du tout**.
`pad_test.cpp` disait s'en remettre à un tableau `absent[]` qui n'a jamais été
écrit, si bien que les quatre restaient éteints en permanence sur matériel —
c'est-à-dire exactement la lecture « le test ne les voit pas » que le testeur
sert à détecter.

Vérifications : table des capacités sortie au préprocesseur pour les trois
cibles (la Switch ne perd aucune ligne, le bureau en perd deux vérifiées
inertes, la Vita cinq) ; branche Nintendo de `pad_draw.cpp` comparée à `HEAD`,
identique à l'octet près (60 lignes, 3265 octets) ; `.vpk` reconstruit sans
avertissement.

## 2026-09-13 (2) — l'échelle a répondu, et le problème n'était pas la vidéo

Le premier essai console de l'échelle descendante a produit exactement ce pour
quoi elle avait été écrite : une réponse, pas une confirmation.

```
sceVideodecInitLibrary(1920x1088, 3 refs) FAIL rc=0x80620802 INVALID_PARAM
13369344 B of decoder memory REFUSED
11010048 B of decoder memory REFUSED
11010048 B of decoder memory REFUSED
 6815744 B of decoder memory REFUSED
 3407872 B of decoder memory REFUSED
```

Lecture : 1080p est bien refusé sur la **géométrie**, mais à tous les échelons
inférieurs `sceVideodecInitLibrary` **réussit** — aucune ligne FAIL — et c'est
notre **allocation** qui échoue. Une demande de 3,25 Mo qui échoue aussi
durement qu'une de 12,75 Mo n'est ni de la fragmentation ni une limite de
décodeur : le pool est vide, ou la demande est malformée. Le problème n'a jamais
été dans le code vidéo.

Deux causes candidates, et le correctif ne tranche pas entre elles — il fait
répondre la console.

**Le grain.** L'arrondi était à 256 Ko, sur un commentaire affirmant « Sce
allocations are granted in 256 KB units for this type ». Rien n'avait vérifié
cette affirmation, et **les cinq tailles refusées sont toutes multiples de
256 Ko et aucune de 1 Mio**. Les blocs PHYCONT sont accordés par 1 Mio. Ce que
le vieux commentaire a coûté mérite d'être noté : il se lisait comme un fait
établi alors que c'était une supposition, ce qui rendait les tailles du journal
plausibles tout en étant indemandables.

**Le budget.** Une application Vita déclare son budget mémoire dans `param.sfo`,
et le nôtre passe `ATTRIBUTE2=12` (« max heap size », hérité du CMake de la démo
Borealis). Ces modes échangent les pools entre eux, donc le tas maximum peut se
payer sur PHYCONT — un budget nul refuserait 3,25 Mo exactement comme 12,75.
`sceKernelGetFreeMemorySize` tranche en une ligne, journalisée une fois avant la
première demande : user / cdram / **PHYCONT**. Un zéro à côté de deux chiffres
sains dit « budget », pas « machine à court de mémoire ».

Et plutôt que s'arrêter à PHYCONT, un **second** pool physiquement contigu est
tenté : CDRAM, les 128 Mo que voit le GPU. Le journal nomme le pool qui a servi
le bloc, donc la réponse survit à la session.

### Le repère de build mentait, et c'est corrigé

Le journal console annonçait `af0a8a1 02:04:52Z` alors que tournait du code
commité deux heures plus tard — la preuve de l'échelle dans le même fichier.
Cause : `add_dependencies(${PROJECT_NAME} shadow_build_id)` était **coincé dans
le `if (NOT USE_LIBROMFS)` de la branche Switch/bureau**, mal indenté. La
branche PSV ne l'atteint jamais. La cible est `ALL` donc elle tournait
toujours — simplement pas forcément avant l'unité de compilation qui inclut
l'en-tête.

Reposée une fois, inconditionnelle, en fin de fichier. **Et cette correction
était insuffisante — voir plus bas (2026-09-13, dixième entrée) :** une
dépendance d'ORDRE ne rend personne sale, et la vérification employée
(`build_id.h` contre `HEAD`) portait sur le seul fichier qui a toujours raison.
**Un repère qui peut être faux est pire que pas de repère** — il se lit comme
la preuve de quel binaire a tourné.

Au passage, méthode : le premier `cmake .` de cette session avait échoué
silencieusement (`VITASDK` absent du shell) derrière un `>/dev/null 2>&1 &&`, et
le build suivant a tourné sur l'ancien ninja en rapportant un succès. Même
famille que le `tail` dont je lisais le code de sortie.

## 2026-09-13 (3) — le décodeur s'ouvre ; c'était le grain, pas le budget

```
free memory - user 178176 KB, cdram 80896 KB, PHYCONT 20480 KB
13312 KB from PHYCONT          <- 12,75 Mo arrondis a 13 Mio
 2048 KB from PHYCONT
hardware decoder ready - 1280x720, 4 ref frame(s)
```

**20 Mo de PHYCONT étaient libres depuis le début.** La mesure a écarté
l'hypothèse du budget en une ligne : `ATTRIBUTE2=12` n'y était pour rien, la
demande était simplement indemandable parce qu'arrondie à 256 Ko au lieu de
1 Mio. Le pool CDRAM n'a pas eu à servir. 1080p reste refusé sur la géométrie,
et l'échelle s'arrête donc à 1280×720/4 refs — exactement ce que la session
négocie en portable.

### Le mur suivant : `wolfSSL_new FAIL`, et le message accusait la mauvaise cause

C'est la première session qui dépasse le décodeur, donc la première qui atteint
la couche TLS. Quinze tentatives, toutes ainsi :

```
ctrl_tcp: connected v4 [<VM public IPv4>]:9011 NODELAY=on
ctrl_tcp: wolfSSL_new FAIL
...
M8.fail — tcp open FAILED after 15 attempts - zombie VM or a different port base
```

La socket **était connectée**. Le port était ouvert, la VM vivante, et le
message envoyait le lecteur au seul endroit où le défaut n'était pas.

**Cause : `sceKernelGetRandomNumber` est limité à 64 octets** (`psp2/kernel/rng.h`
le dit dans son `@param`), et `wolfssl_rand_switch.c` lui passait `sz` tel quel,
sous un commentaire admettant « NOT VALIDATED ON HARDWARE ». wolfSSL semait sa
DRBG et n'obtenait rien, donc `wolfSSL_new` rendait NULL. À noter, parce que
c'est le motif et pas l'accident : **la branche Linux de ce même fichier, trois
lignes plus bas, boucle déjà sur `getrandom`.** Le même auteur a écrit les deux
et n'en a bouclé qu'une — une limite par plateforme est précisément ce qu'un
appel unique non vérifié dissimule.

`ctrl_tcp` retient désormais **l'étape** qui a échoué (`TCP connect`,
`wolfSSL_new (entropy or memory)`, `TLS handshake`…) et `M8.fail` la cite au
lieu d'affirmer une cause qu'il ne peut pas connaître. Un diagnostic qui nomme
une cause fausse coûte plus cher que pas de diagnostic.

Et `%zu` s'imprimait littéralement `zu` (« step zu of the ladder ») : la newlib
de cette chaîne ne porte pas le modificateur. Le journal étant la preuve, une
conversion silencieusement perdue est un défaut, pas un détail.

L'avertissement `-Wformat-truncation` de `ctrl_session.c:1075` est préexistant
(la ligne `[AUD2]`), hors de cette modification.

## 2026-09-13 (4) — la graine n'était branchée sur rien

`wolfSSL_new FAIL` a survécu au découpage à 64 octets, donc l'hypothèse était
juste et le correctif au mauvais endroit. Ce qui l'a tranché n'est pas une
relecture du code, c'est `nm` :

```
notre objet          : T switch_rand_seed          (et PAS wc_GenerateSeed)
libwolfssl.a (Vita)  : T wc_GenerateSeed           (le sien, lisant /dev/urandom)
libwolfssl.a (Switch): 0 reference a /dev/urandom  (le notre, depuis toujours)
```

`core/services/wolfssl_rand_switch.c` fournit `switch_rand_seed` depuis le début
du port, et **sa toute première ligne** dit qu'elle est « handed to wolfSSL
through `CUSTOM_RAND_GENERATE_SEED` ». Aucun build ne passait cette macro. Le
symbole n'était donc référencé par rien : du code mort qui se lisait comme une
intégration. wolfSSL conservait sa propre graine, la Vita n'a pas de
`/dev/urandom`, `wc_InitRng` échouait, `wolfSSL_new` rendait NULL — quinze fois,
sur une socket connectée.

**Deux symboles censés être la même chose, et aucun des deux builds ne s'est
plaint.** Un commentaire affirmant un câblage n'est pas un câblage ; ce qui le
prouve est un lien, et ce qui le vérifie est `nm`. Même famille que le grain de
256 Ko de ce matin : une phrase juste-sonnante tenant lieu de fait vérifié.

Le découpage à 64 octets n'était pas inutile pour autant — il était correct et
inerte, et il devient nécessaire dès que la fonction est réellement appelée
(`sceKernelGetRandomNumber` refuse au-delà, `psp2/kernel/rng.h`).

## 2026-09-13 (5) — le bootstrap passe en entier ; le crash venait du journal

Le TLS fonctionne et **la session va jusqu'au bout** pour la première fois :

```
M8.found -> M9 Capabilities 46 ms -> M10 Authentication 574 ms -> M11 Encryption 27 ms
[K12] announcement grants received: 8 of 8
udp_register :12010 OK, :12030 OK, :12013 socket
vst: ctrl_video_tcp_open OK on :12020
```

Puis Data abort. Le dump le dit exactement — `strlen`, appelé depuis
`_svfprintf_r`, avec `R1 = 0x500` :

```
Stop reason: 0x30004 (Data abort exception)
PC: 0x816b4de4  ->  strlen+0x24 :  ldrd r2, r3, [r1]
LR: 0x816b84df  ->  _svfprintf_r+0x1376  (juste apres `bl strlen`)
pile : '%zu hex=%s'   <- la chaine de format
```

**Cause racine, au niveau de la bibliothèque** : `newlib.h` du vitasdk, ligne 18,
`/* #undef _WANT_IO_C99_FORMATS */`. Cette newlib ne connaît ni `%z`, ni `%j`,
ni `%t`. Un modificateur inconnu fait imprimer les lettres — d'où
« step zu of the ladder », `len=zu`, `sent=zd` — **et ne consomme pas son
argument**, si bien que toute conversion suivante lit le mauvais emplacement de
la `va_list`. `vid_reasm.c:2192` a donc passé `0x500` à `strlen`, sur le
**premier paquet vidéo** : c'est-à-dire au moment précis où le chemin de l'image
s'est enfin mis à fonctionner.

Il y avait **106 `%z` dans ce dépôt, dont 42 suivis d'une autre conversion** :
42 crashs latents, pas un défaut isolé. Et rien ne pouvait les signaler — `%zu`
est du C99 valide, donc `-Wformat` se tait.

Corrigé **en un point**, dans `journal.c`, parce que 105 des 106 passent par le
journal : la chaîne de format est réécrite avant `vsnprintf` pour retirer le
`z`. **La condition est la libc, pas la plateforme** — `#ifdef __vita__` aurait
été l'habitude soustractive que ce port paie depuis le début, et serait devenu
faux le jour où vitasdk activera le drapeau. `newlib.h` publie la réponse, donc
on la lui demande (`SHADOW_LIBC_LACKS_Z_FORMAT`). Là où la libc gère C99, tout
ceci compile à zéro.

La réécriture n'est correcte que parce qu'elle est bornée à cette libc : `size_t`
y est `unsigned int` (ARM 32 bits), donc retirer le `z` transforme `%zu` en
`%u`, qui consomme exactement les quatre octets poussés. Sur une cible 64 bits
la même édition serait fausse — raison de plus pour ne pas conditionner par
plateforme.

**Le test a rattrapé un défaut dans le correctif avant la console.** Le
raccourci d'origine cherchait la paire `"%z"` et ratait donc `%8zu`, `%*zu`,
`%.3zu`, où une largeur s'intercale : ces formats repartaient inchangés. La
fonction réelle a été extraite et éprouvée sur neuf cas plus un rendu complet
(`len=%zu hex=%s` + `(7, "OK")` -> `len=7 hex=OK`) ; deux échecs au premier
passage, zéro après. Sans cela c'était un second aller-retour console.

Reste un site hors journal, corrigé à la main (`ctrl_session.c:1063`,
`fprintf(stderr, …)`), et **pas de garde automatique** contre un futur `%z`
écrit dans un `snprintf` direct : `-Wformat` ne peut pas aider, et un détecteur
fiable devrait comprendre les appels multi-lignes. C'est une dette assumée.

## 2026-09-13 (6) — du son, et il était brouillé pour une raison précise

Première session complète avec du flux : **577 images décodées par le matériel**
(`idr_t=5 sps=5`), 4357 paquets audio, 0 erreur de décodage. Image noire —
attendu, le renderer GXM n'existe pas — et **du son, brouillé et très
grésillant**.

Ce qui a désigné le coupable, c'est que **tous les compteurs étaient sains** :

```
rate 1.17 (10 s) puis 1.00      <- le débit se stabilise, ce n'est pas la source
anneau_plein=42 a t=10s ET a t=20s   <- FIGE : les 42 debordements sont au demarrage
ecartees=7   erreurs=0   aud_stale=0
[L5] audio/file p50=53.2 ms stable
```

Un compteur figé est une information : après la mise en route, plus un seul
débordement, plus un seul rejet. Le défaut était donc **sous** tout ce que le
pipeline mesure, dans la seule partie neuve que rien n'observe — la sortie
Vita.

**`sceAudioOutOutput` ne copie pas le tampon qu'on lui passe.** Elle rend la
main quand le grain *précédent* a fini de jouer, ce qui veut dire que le grain
qu'on vient de soumettre est celui que le matériel est en train de lire. Avec un
accumulateur unique, on recopiait les échantillons suivants **par-dessus ceux en
cours de lecture**. D'où un son continûment brouillé, sans qu'aucun compteur ne
puisse le voir : du point de vue du pipeline tout avait été écrit correctement.

Deux grains alternés, c'est le minimum : pendant que l'appareil joue `acc[cur]`,
on remplit `acc[cur ^ 1]`. Le basculement se fait **avant** de rendre la main à
l'appelant, le tampon soumis appartenant au matériel jusqu'au retour de l'appel
suivant.

Au passage, `if (!o->acc)` portait sur un tableau depuis le changement : une
condition toujours fausse. Corrigée en testant les deux allocations.

## 2026-09-13 (7) — le renderer GXM : aucun shader écrit

Le son est confirmé propre sur matériel après le double tampon. Reste l'image.

**Ce renderer n'est pas un port de celui en GL, et c'est délibéré.**
`gl_video_renderer.cpp` fait ce que fait un bureau : il **sort** de nanovg
(`nvgEndFrame`), pose son programme, deux textures, un quad et un shader
YUV→RGB, puis réouvre une trame. Reproduire ça sur GXM demanderait un shader
patcher, des programmes vertex et fragment, de la mémoire USSE et une cible de
rendu — en doublant ce que Borealis possède déjà, et en pouvant laisser le GPU
dans un état que Borealis n'attend pas.

La Vita y arrive sans rien de tout ça, pour deux raisons qui sont des
propriétés du matériel et non des astuces :

1. **L'unité de texture convertit le YUV elle-même.**
   `SCE_GXM_TEXTURE_FORMAT_YUV420P2_CSC*` est un format 4:2:0 à deux plans —
   NV12, exactement ce que rend `SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER` —
   et l'échantillonner retourne du RGB. La conversion à laquelle le chemin GL
   consacre un shader fragment est **gratuite** ici : il n'y a pas de shader à
   écrire.

2. **Le backend nanovg de Borealis sait adopter une texture qu'on a bâtie.**
   `nvgxmCreateImageFromHandle` transforme un `SceGxmTexture` en identifiant
   d'image, marqué `NODELETE` donc la propriété reste chez nous. À partir de là
   l'image est un `nvgImagePattern` ordinaire, dessiné **dans** la trame de
   Borealis, dans le même espace de coordonnées que le reste — ce qui est aussi
   pourquoi ce renderer prend des coordonnées logiques et aucun viewport, là où
   celui en GL a besoin des deux et d'une multiplication par `windowScale` (V1).

**Trois tampons, et c'est la leçon que le backend audio a payée le même jour** :
les dessins GXM sont **différés**. La texture est lue quand le GPU exécute la
liste de commandes, bien après le retour de `render()` ; écrire les pixels de
l'image suivante dans le tampon que le GPU échantillonne encore déchire
l'image. On cycle.

Points techniques réglés en passant :

- `gxmCreateTexture` de Borealis calcule `stride * height` avec un nombre
  **entier** d'octets par pixel : il sous-alloue un format à 1,5 octet/pixel.
  L'allocation est donc faite directement avec `gpu_alloc_map`, LPDDR d'abord
  et CDRAM en repli — l'ordre que l'allocateur de textures de nanovg emploie
  lui-même.
- La texture fait la largeur de la **foulée**, pas de l'image : seuls
  `frame_w × frame_h` sont affichables, et c'est le motif qui garde les
  colonnes d'alignement hors de l'écran (le défaut de bande verte que tout port
  de ce genre rencontre une fois).
- La copie par ligne est **bornée à la foulée source**. `pitch_` vaut
  `y_stride` par construction, mais rien ne garantit que le plan chroma porte
  la même foulée, et lire `pitch_` octets d'une ligne plus courte sortirait du
  tampon de l'appelant.
- `file(GLOB_RECURSE)` donne ce `.cpp` à **toutes** les cibles : il est donc
  entièrement gardé par `SHADOW_HAS_GXM_VIDEO` et ne rend aucun symbole
  ailleurs. Et il a fallu **reconfigurer** cmake pour que le glob voie le
  nouveau fichier — le lien échouait sur trois symboles absents alors que la
  compilation passait.

`SHADOW_GXM_CSC` choisit la matrice. COL1 a mesuré que le flux est du **BT.709
plage limitée**, donc CSC1 par défaut, comme dans le shader GL — mais
**l'appariement CSC0/CSC1 ↔ 601/709 est déduit des noms de format, pas mesuré
sur cette machine**. Si les couleurs sortent fausses ou délavées, c'est la
première chose à essayer, et la réponse appartient à `KB §9` à côté de COL1.

Non validé sur matériel à cette heure : que l'alpha sorte à 1 de la conversion
(une texture YUV dont l'alpha vaudrait 0 ne dessinerait rien), et le coût de la
copie de 1,4 Mo par image sur ce processeur. La ligne
`gxm/video: first picture drawn` répond à la première question.

## 2026-09-13 (8) — l'image sort ; couleur, tactile et boutons

**Il y a une image.** Trois défauts rapportés dans la foulée, et chacun avait
une cause différente.

### Le bleu virait au rouge — U et V échangés

Le symptôme nomme sa cause : échanger Cb et Cr intervertit exactement ces deux
couleurs, là où une mauvaise **matrice** (601 contre 709) décale les teintes et
la saturation sans jamais transformer le bleu en rouge. L'ordonnancement « YUV »
de GXM lit donc le premier octet chroma comme V, tandis que
`SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER` y écrit U — c'est du NV12.
`YVU420P2` est le même format lu dans l'autre sens, et devient le défaut.

Les deux axes restent **indépendants** et gardent chacun leur bascule, parce
que la première exécution console n'en a tranché qu'un :

```
SHADOW_GXM_CHROMA=0   revient a l'ordre YUV (l'inversion bleu/rouge)
SHADOW_GXM_CSC=0      BT.601 au lieu de BT.709
```

La matrice reste **déduite**, pas mesurée.

### Le tactile ne faisait rien — il n'était jamais lu

Deux endroits, deux fois le même motif. `ui/touch.h` disait « Off console there
is nothing to read » sous un `#ifdef __SWITCH__` — vrai quand la Switch était la
seule console. Et `stream_view.cpp` portait **274 lignes** d'entrée en cours de
flux dans un autre `#ifdef __SWITCH__`, plus deux blocs de 204 et 21 lignes :
le tactile-souris et la navigation du menu de pause étaient purement absents du
binaire.

Aucun de ces blocs n'a été réécrit. Leur surface libnx est de **huit points
d'entrée et huit constantes** — la croix, le stick gauche, Plus/Minus, les
clics de stick, trois lectures tactiles et une horloge monotone ;
`include/pad_compat.h` les fournit, adossés à Borealis. Le reste est de la
logique neutre, et elle reste **identique à l'octet près** sur les deux
consoles : la vérification du diff ne montre que les lignes de garde et un
include.

Ce qui rend ce shim correct plutôt que commode : **Borealis convertit déjà** le
panneau tactile via l'aire active de `SceTouchPanelInfo`, vers
`Application::contentWidth/Height` — soit le même espace 1280×720 que remonte
libnx sur Switch. Les coordonnées passent telles quelles et la logique
au-dessus ne peut pas distinguer les deux machines. Réécrire cette conversion
ici aurait été le chemin dupliqué que ce dépôt paie en boucle.

Les bits de bouton sont les **vraies valeurs libnx**, délibérément :
`padGetButtons()` est combiné par OU avec `devlink::injectedNpadMask()`, qui
parle libnx. Inventer des bits distincts aurait marché jusqu'au jour où devlink
injecte une direction.

### SELECT et START : oui, câblés, et de la même façon que sur Switch

`psv_input.cpp` mappe `SCE_CTRL_START` sur `BUTTON_START` et `SCE_CTRL_SELECT`
sur `BUTTON_BACK`, et `stream_activity.cpp` enregistre une action **silencieuse**
sur ces deux-là uniquement pour empêcher Borealis de les avaler : la vraie
logique appui court / appui long vit dans `StreamView::draw()`, qui interroge
`padGetButtons`. C'est-à-dire dans les blocs qui n'étaient pas compilés. Ils le
sont maintenant, et le shim mappe Plus et Minus sur START et SELECT.

Les **clics de stick** sont lus plutôt qu'ignorés : une Vita de poche n'en a
pas, donc ils restent faux et la combinaison qui les demande ne se déclenche
jamais — mais une PlayStation TV avec une DualShock les fournit, et ce qui
manque là-bas est le matériel du portable, pas l'API.


## La boucle d'itération — et pourquoi le `.vpk` n'est pas le bon fichier

`tools/vita-sync.sh` remplace le build sur la carte et la rend.

```bash
tools/vita-sync.sh              # eboot.bin + demontage  (le defaut)
tools/vita-sync.sh vpk          # depose le .vpk dans downloads/ (premiere install)
tools/vita-sync.sh full         # eboot + resources/, quand les resources bougent
tools/vita-sync.sh eboot --keep # sans demonter
```

**Le `.vpk` est un installeur, pas une mise à jour.** Le déposer dans
`ux0:data/downloads/` coûte encore un passage par VitaShell, une confirmation,
une invite d'écrasement et une reconstruction de la bulle — pour 9,4 Mo. Or le
titre est déjà installé, et ce qui change entre deux builds tient dans **un
fichier** : `ux0:app/HLYD00001/eboot.bin`, 5,3 Mo, qui est exactement
`halyard.self` sous un autre nom. Le remplacer *est* la mise à jour.

Le point de montage est **découvert**, jamais supposé : le chemin
`/media/<user>/<label>` est celui de cette machine-ci, et le script cherche la
carte qui porte `app/$TITLE_ID`. Et il fait `sync` avant de démonter — une carte
retirée pendant que la FAT est encore sale donne une application « corrompue »
côté console, ce qui se diagnostique très mal puisque le vrai défaut est une
copie jamais terminée.

### VitaShell, et ce qui vaut mieux qu'un gestionnaire de fichiers

VitaShell **est** le bon gestionnaire de fichiers : c'est le standard de fait,
le plus complet et le plus maintenu ; molecularShell est son ancêtre et n'est
pas plus stable. Il n'y a pas de meilleure app dans cette catégorie.

Mais pour cette boucle, la bonne réponse est de **ne plus passer par un
gestionnaire de fichiers** :

- **Le FTP intégré de VitaShell** (touche SELECT) évite déjà le débranchement —
  mais il faut que VitaShell soit ouvert au premier plan.
- **`vita-companion`** (greffon taiHEN) fait tourner un démon FTP en
  permanence, en arrière-plan, plus un canal de commandes qui sait **lancer un
  titre par son title ID**. La boucle devient : construire → envoyer
  `eboot.bin` → `launch HLYD00001`, sans un geste sur la console. C'est
  l'équivalent Vita de ce que RELOAD-1 a apporté à la Switch (18 s par tour,
  dont 3 de redémarrage).

Non vérifié à cette date : que `vita-companion` s'installe et fonctionne sur ce
firmware. C'est une recommandation, pas une mesure.


## 2026-09-13 (9) — PSV3 : les sticks étaient bloqués à gauche

Couleurs confirmées justes sur matériel après l'inversion U/V. Défaut suivant :
dans le menu de pause, « le bouton gauche reste appuyé en permanence » et les
valeurs défilent à toute vitesse dès qu'on entre dans une entrée.

Ce n'était pas le shim, c'était Borealis :

```c
state->axes[LEFT_X] = pad.lx / 255.0f - 1.0f;      // psv_input.cpp
```

`pad.lx` va de 0 à 255, centre 0x80. Le diviseur 255 projette donc cette course
sur **−1..0** au lieu de −1..+1 : au repos l'axe vaut `128/255 − 1 = −0.498`, et
un stick poussé à fond à droite ne peut jamais dépasser 0. Le centre étant à
127.5, c'est 127.5 le diviseur.

Le compte est exact : le menu de pause seuille à `THRESHOLD = 16000`, et
−0.498 × 32767 = **−16318** — juste au-delà, donc `dx = -1` à chaque image. Après
correction le repos donne +128, très loin du seuil.

**Pourquoi personne ne l'avait vu.** La navigation propre à Borealis ne lit pas
ces axes : `BUTTON_NAV_*`, calculé quatre lignes plus bas, part des valeurs
BRUTES `pad.lx`/`pad.ly` avec ses propres seuils. Les axes n'étaient donc faux
que pour un consommateur qui les lit, et jusqu'à ce que le menu de pause du flux
atteigne cette console il n'y en avait aucun. C'est la même forme que le reste
de la journée : un chemin correct et un chemin faux côte à côte, et seul celui
qu'on n'exerçait pas était cassé.

Corrigé dans l'arbre vendu (PSV3) **et dans `local-fixes.patch`** : cet arbre
n'est pas suivi par git, il est cloné par `bootstrap-libs.sh` puis patché — une
correction laissée seulement sur disque ne survivrait pas à un clone neuf. Le
hunk a été vérifié en l'appliquant sur une copie propre de l'original.
