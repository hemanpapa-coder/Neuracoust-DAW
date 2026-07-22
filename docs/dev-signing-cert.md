# Stable dev code-signing certificate (TCC grants that survive a rebuild)

The SwiftUI app needs a real code identity so macOS shows the microphone prompt and remembers
privacy grants. **Ad-hoc signing (`codesign -s -`) is not enough for grants that persist**: ad-hoc's
designated requirement is the binary's cdhash, which changes on every build, so TCC treats each
rebuild as a brand-new app. Every rebuild you would have to re-grant:

- **Accessibility** — the global keypad capture (`GlobalKeypadCapture` / CGEventTap)
- **Input Monitoring / Microphone** — BlackHole reference monitoring, talkback, measurement

A **self-signed certificate** fixes this: the requirement becomes
`identifier "com.neuracoust.daw" and certificate leaf = H"…"`, which is stable across rebuilds as
long as the bundle id and the cert stay the same. `CMakeLists.txt` auto-detects a login-keychain
cert named **`Neuracoust Dev Signing`** and signs with it; if absent it falls back to ad-hoc.

## Create it once (per machine)

Either use Keychain Access → Certificate Assistant → *Create a Certificate…* (Name:
`Neuracoust Dev Signing`, Identity Type: Self-Signed Root, Certificate Type: **Code Signing**), or
the CLI:

```sh
cd "$(mktemp -d)"
cat > ext.cnf <<'EOF'
[req]
distinguished_name = dn
x509_extensions = v3
prompt = no
[dn]
CN = Neuracoust Dev Signing
[v3]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
EOF
openssl req -x509 -newkey rsa:2048 -keyout k.key -out c.crt -days 3650 -nodes -config ext.cnf
openssl pkcs12 -export -inkey k.key -in c.crt -out c.p12 -passout pass:ncdev -name "Neuracoust Dev Signing"
security import c.p12 -k ~/Library/Keychains/login.keychain-db -P ncdev -T /usr/bin/codesign
```

The cert does **not** need to be marked "trusted" — `codesign` signs with it by identity/hash
regardless (trust only affects *verification*, and the app is run locally, unquarantined). After
`cmake` reconfigures, it will find the cert and use it.

## After switching to the stable cert (one-time)

Because the code identity changed, you must re-grant once more, then it sticks:

1. System Settings → Privacy & Security → **Accessibility** (and **Input Monitoring**): remove any
   old "Neuracoust DAW" entry, then re-add / re-enable the freshly built app.
2. Fully quit and relaunch the app (TCC trust is cached per process).

From then on, rebuilds keep the grant.
