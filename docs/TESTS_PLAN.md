# Visual test plan — halyard v1

> ⚠️ **What you are about to capture contains your credentials.** A Shadow
> session carries `Authorization: Bearer` JWTs, a streaming token, and - on the
> SFTP channel - an ed25519 private key the server issues. A capture, a keylog
> or a request dump holds all of it in the clear. Keep the output outside the
> repository, do not attach it to an issue, and delete it when you are done.
> See [`SECURITY.md`](../SECURITY.md).

24 commits delivered. Here is an **exhaustive checklist** for validating every
improvement and feature before the final push / the live Switch test.

> The UI wordings quoted below are what the screen showed at v1. The app is
> localised (`resources/i18n/`), so the exact sentence depends on the active
> locale; what matters is that the screen says that *thing*, not those bytes.

---

## 🔧 Setup before testing

```bash
cd $REPO
# Build the latest
cd build_linux && make -j$(nproc) halyard halyard-cli && cd ../..

# Live logs in parallel (terminal 2)
tail -f /tmp/halyard/webrtc.log
```

**Prerequisites**:
- An active internet connection
- A Shadow account already logged in (= `/tmp/halyard/refresh_token` exists)
- A reachable Shadow VM

---

## 1️⃣ Boot + Auth (= UX1+UX2+UX3)

### Test 1.1 — Normal boot (= the happy path)
```bash
cd $REPO/build_linux
./halyard
```

**To check**:
- [ ] The "Shadow PC" splash then the Borealis logo
- [ ] The message "Checking internet connection…" (= UX1 A6)
- [ ] The message "Looking for the Shadow datacenter…"
- [ ] The message "Authenticating (OAuth)…"
- [ ] The VM list appears, with VM cards

### Test 1.2 — No internet (= UX1+UX2 A4)
Disable Wi-Fi/networking on Linux, then:
```bash
./halyard
```

**To check**:
- [ ] The error **"❌ No internet access detected"** with Wi-Fi/captive-portal/DNS
      instructions
- [ ] No infinite hang (= a ~3 s timeout)

### Test 1.3 — Datacenter temporarily down (= UX1 A3 retry)
Block api.shadow.tech through `/etc/hosts`:
```bash
echo "127.0.0.1 api.shadow.tech" | sudo tee -a /etc/hosts
./halyard
```

**To check**:
- [ ] The message "Retry TINAG (1/3) in 2s…"
- [ ] The message "Retry TINAG (2/3) in 4s…"
- [ ] The message "Retry TINAG (3/3) in 8s…"
- [ ] After 3 retries: a clear diagnostic error

```bash
# Cleanup
sudo sed -i '/api.shadow.tech/d' /etc/hosts
```

### Test 1.4 — Expired-token simulation
Corrupt the refresh token:
```bash
mv /tmp/halyard/refresh_token /tmp/halyard/refresh_token.bak
echo "invalid_token_garbage" > /tmp/halyard/refresh_token
./halyard
```

**To check**:
- [ ] The app detects the refresh failure
- [ ] It shows the Device Grant QR code (= the re-auth flow)
- [ ] The user can rescan to recover access

```bash
# Restore
mv /tmp/halyard/refresh_token.bak /tmp/halyard/refresh_token
```

### Test 1.5 — Token obfuscation v2 (= UX3 B1)
After a normal boot:
```bash
xxd /tmp/halyard/refresh_token | head -3
```

**To check**:
- [ ] **First byte = `0x52`** (= the v2 magic)
- [ ] The rest is XOR-obfuscated bytes (= not readable JSON)
- [ ] If you read the file as plaintext, you see binary, not the JWT

---

## 2️⃣ VM list (= UX2+UX4)

### Test 2.1 — VM display + account info
After a normal boot, on the VM list screen:

**To check**:
- [ ] VM cards displayed with alias/state/colours
- [ ] **Plan label**: "Plan: Power 2023 — active since 15/03/2025" (= UX4 B12)
- [ ] **Drive label**: "Drive: ✓ enabled" or similar
- [ ] On Switch with battery < 15 %: "⚠ Battery X% (plugging in recommended)"
      (= UX4 B9)

### Test 2.2 — Quality settings (= the Y button → UX5 B6 + UX2 B8)
Press **Y** on the VM list:

**To check**:
- [ ] The "Stream quality" screen opens
- [ ] 5 SelectorCells visible: Bitrate / Framerate / Resolution / Codec / Profile
- [ ] Select a new bitrate (= e.g. 50 Mbps)
- [ ] A **"✓ Settings saved" toast** appears briefly (UX2 B8)
- [ ] Back with B → the settings persist (= relaunch the app, check the value is
      still there)

### Test 2.3 — Logout button (= UX2 A5)
Press **LB** (left bumper) on the VM list:

**To check**:
- [ ] The dialog "Log out of Shadow?" + explanatory text
- [ ] The buttons "Cancel" + "Log out"
- [ ] Cancel → the dialog closes, back to the VM list
- [ ] Log out → the app exits
- [ ] On the next launch → the Device Grant re-auth flow

---

## 3️⃣ Connecting (= UX5 B6 + UX1 A4 errors)

### Test 3.1 — Normal connect
Click a VM in the list → ConnectingActivity is pushed

**To check**:
- [ ] **Prefixed titles**: "[1/7] Start the VM" → "[7/7] WebRTC handshake"
- [ ] Visual states: pending → running → done (= icons + colours)
- [ ] The steps progress in sequence

### Test 3.2 — An error on an intermediate step
Cut the Wi-Fi during connection:

**To check**:
- [ ] The current step → Error state with the HTTP/error message
- [ ] No hang: a ~30 s timeout at most
- [ ] The B button → back to the VM list

---

## 4️⃣ Stream (= UX2+UX5 stats + cursor)

### Test 4.1 — A basic live stream
Connect to a VM, wait for the stream to go live:

**To check**:
- [ ] Video displayed (= a stable 30 fps, bottom NAL 50 %)
- [ ] A **WHITE crosshair** at the centre (= the local position WE send)
- [ ] A **CYAN circle** at the Shadow VM's position (= CUR1 phase 2, if cursor
      frames are received)
- [ ] No static green bands
- [ ] The stats overlay hidden by default

### Test 4.2 — Stats overlay (= UX5 B2)
Press **Plus (+)** to open the menu → "Toggle stats":

**To check**:
- [ ] A stats panel at the top right with sections:
  - PERFORMANCE (FPS / Lat / Int)
  - NETWORK (RTP pkts/s + a **colour-coded live Mbps bitrate** = UX5 B2)
  - VIDEO (resolution + format)
  - INPUT debug
- [ ] **Bitrate Mbps colour**:
  - 🟢 Green if >=4 Mbps
  - 🟡 Yellow if >=1 Mbps
  - 🔴 Red otherwise

### Test 4.3 — Disconnect dialog (= UX2 B7)
The stream menu (+) → select **MENU_QUIT**:

**To check**:
- [ ] The dialog "Leave the stream? The Shadow VM keeps running…"
- [ ] The buttons "Keep streaming" + "Leave"
- [ ] Keep streaming → the stream resumes
- [ ] Leave → a clean signalAbort, back to the VM list

### Test 4.4 — Runtime quality params (= Q1+RE3+RE8)
Set the quality settings pre-stream + connect:
```bash
# Without the GUI: use the env var directly
SHADOW_BITRATE_MBPS=80 SHADOW_FPS=120 ./halyard
```

**To check in webrtc.log**:
- [ ] `[Q1] channel video params: 1920x1080 @ 120.0 fps @ 80.0 Mbps codec=2 profile=1 re8=[00000]`
- [ ] `[Q1+C] VideoEncodingConfig : bitrate=80.0 Mbps fps=120.0`
- [ ] The stream starts normally

### Test 4.5 — Testing the RE8 booleans (= optional, risky)
```bash
# Field 10 in isolation (= already done, breaks the stream)
SHADOW_REG_F10=1 ./halyard
# -> expected: bootstrap OK but 0 frames (= the server changes mode)
```

---

## 5️⃣ Native channels (= V16 + I1 + CUR1)

### Test 5.1 — VST channel :base+20 (= V16 + RE11)
During a stream, in webrtc.log:

**To check**:
- [ ] `vst: TLS handshake OK cipher=TLS_AES_256_GCM_SHA384`
- [ ] `vst: 3x 0x64 trigger sent (rc=1/1/1)`
- [ ] `vst: heartbeat 0x70 sent (rc=1, period=7000ms)` every ~7 s
- [ ] **No FIN from the server** (= the connection is alive)
- [ ] If VST receives frames: `[RE11] VST → h264 feed nal_type=X len=Y` appears

### Test 5.2 — Input channel :base+14 (= I1 + RE5)
**To check**:
- [ ] `[input-tcp] TLS handshake OK`
- [ ] `[input-tcp] Connect 96B sent`
- [ ] If you move the mouse: the Shadow VM's cursor may now move!
  - (= RE5 X@128-129, Y@130-131, a byte-exact patch validated on 250 desktop
    samples)

### Test 5.3 — Cursor channel :base+30 (= CUR1)
**To check in webrtc.log**:
- [ ] `[cursor] init mode=abs_centered (1920x1080)`
- [ ] `[cursor] frame #N fmt=raw len=271` (the initial bitmap)
- [ ] `[cursor] bitmap update #1 (271 B)`
- [ ] `[cursor] pos update #N raw=(X,Y) → (948,538)` (= a stable tracked position)

### Test 5.4 — Cursor dump for format analysis
```bash
SHADOW_DUMP_CURSOR=1 ./halyard
# Stream briefly, then exit
ls -la /tmp/cursor_*.bin  # -> 10 .bin files captured
```

---

## 6️⃣ Security & privacy (= UX3 B1 + UX7 B11)

### Test 6.1 — Log redaction (= UX7 B11)
After a complete boot/stream:
```bash
grep "Bearer" /tmp/halyard/webrtc.log | head -5
grep "refresh_token" /tmp/halyard/webrtc.log | head -3
```

**To check**:
- [ ] Every `Bearer …` is **`Bearer [JWT-REDACTED]`**
- [ ] Every `refresh_token=…` is **`refresh_token=[REFRESH-REDACTED]`**
- [ ] **NO** plaintext JWT visible

### Test 6.2 — Disabling redaction for debugging
```bash
SHADOW_LOG_RAW=1 ./halyard
# Quit quickly
grep "Bearer ey" /tmp/halyard/webrtc.log | head -1
```

**To check**:
- [ ] The **Bearer JWT visible in plaintext** when SHADOW_LOG_RAW=1 (= a deliberate
      bypass)

---

## 7️⃣ Switch-specific (= UX6 B3 — testable only on a Switch console)

### Test 7.1 — Sleep-mode handling
On a Switch console:
1. Launch halyard.nro
2. Connect to a VM, wait for the stream to go live
3. Press the **Power** button to put it to sleep
4. Wait 5 s
5. Wake the console up

**To check (in the logs after waking)**:
- [ ] `[UX6] Switch sleep detected, signal abort` in webrtc.log
- [ ] The app returns cleanly to the VM list, or at least does not crash

### Test 7.2 — Low-battery warning
On a Switch with battery < 15 %:
- [ ] On the VM list: "⚠ Battery X% (plugging in recommended)" visible

---

## 8️⃣ Automated smoke test (= a reminder about auto-test.sh)

To validate the whole chain headless:
```bash
cd $REPO
tools/auto-test.sh 30 native-stream
# Expected: bootstrap OK, fps ~30, bottom ~50 %, decrypt 100 %
```

**To check**:
- [ ] Exit code 0
- [ ] bottom_pct >= 49.5 %
- [ ] decrypt_pct = 100 %

And with a quality boost:
```bash
SHADOW_BITRATE_MBPS=80 SHADOW_FPS=60 tools/auto-test.sh 60 native-stream
```

---

## 📋 Expected / known issues

| Issue | State | Workaround |
|---|---|---|
| The cyan cursor overlay is invisible if the Shadow VM cursor is stationary | Minor | Move the desktop mouse |
| The VST channel is idle when there is no UDP packet loss | Normal | That is by design |
| The battery API is a no-op on Linux | Normal | Test on Switch |
| The sleep handler is a no-op on Linux | Normal | Test on Switch |
| Raw log mode (SHADOW_LOG_RAW=1) leaks the JWT | Deliberate | Do not push logs with it enabled |
| Cursor bitmap pixels not rendered (= just the cyan position circle) | TODO, phase 3 | Needs a non-blank cursor capture |

---

## ✅ v1 acceptance criteria

To push the v1 tag, validate at least **80 %** of the items above:

### Must-have (= a blocker if it fails)
- ☐ 1.1 Normal boot, happy path
- ☐ 2.1 The VM list shows VMs + account info
- ☐ 2.3 The logout button works
- ☐ 3.1 Normal connect, "[N/7]" visible
- ☐ 4.1 The stream shows video + crosshair
- ☐ 4.3 The disconnect dialog works
- ☐ 6.1 Log redaction is active
- ☐ 8 The smoke test exits 0

### Nice-to-have (= P1 if it fails)
- ☐ 1.2 No internet → a clean error screen
- ☐ 1.3 The TINAG retries are visible
- ☐ 1.5 The token is obfuscated (magic 0x52)
- ☐ 2.2 The "✓ Saved" quality toast
- ☐ 4.2 The enriched, colour-coded stats overlay
- ☐ 5.1 VST channel activity in the logs
- ☐ 5.3 The cursor channel is decoded

### Switch-specific (= to test on the console)
- ☐ 7.1 Sleep mode → a clean disconnect
- ☐ 7.2 The battery warning below 15 %

---

## 🚀 What comes after validation

Once v1 is validated:

1. **Live Switch test** (= the pending V15 task): build the NRO + push over FTP +
   run on the console
2. **Git tag**: `git tag v1-baseline + git push --tags`
3. **CHANGELOG.md** + **RELEASE_NOTES.md** for publishing
4. **Next target**: choose among:
   - Q2, an interactive desktop capture (= unlocks the audio PSK, the cursor bitmap
     pixels, real X/Y validation)
   - CUR1 phase 3 (= rendering the cursor sprite instead of a cyan circle)
   - Audio DTLS phase 2 (= identify the PSK + Opus playback)
   - Dock/handheld performance mode (= P2 C3)
   - VM thumbnail previews (= P2 C2)

Good luck with the testing! 🎮
