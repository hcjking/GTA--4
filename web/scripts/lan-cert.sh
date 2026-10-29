#!/bin/bash
# Makes the certificates serve.py's Wi-Fi mode (GTA4_WEB_LAN=1) uses: a local
# certificate authority, made once (the iPad or phone installs and trusts it
# once), and this computer's certificate signed by it, for its current Wi-Fi
# address and its .local name (remade whenever those change).
# Usage: lan-cert.sh <folder> <ip>
set -eu
DIR=$1 IP=$2
NAME=$(scutil --get LocalHostName 2>/dev/null || hostname -s)
mkdir -p "$DIR"
chmod 700 "$DIR"
cd "$DIR"
if [ ! -f ca.key ]; then
  openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -days 3650 -sha256 \
    -subj "/CN=GTA IV Web local CA ($NAME)" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" 2>/dev/null
  rm -f server.crt
fi
# (iOS takes server certificates of at most 825 days, with the name in the SAN
# and serverAuth)
WANT="IP:$IP,DNS:$NAME.local,DNS:localhost,IP:127.0.0.1"
if [ -f server.crt ] && [ "$(cat server.san 2>/dev/null)" = "$WANT" ]; then exit 0; fi
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr -subj "/CN=$NAME.local" 2>/dev/null
printf 'subjectAltName=%s\nextendedKeyUsage=serverAuth\nkeyUsage=critical,digitalSignature,keyEncipherment\nbasicConstraints=CA:FALSE\n' "$WANT" > server.ext
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt -days 397 -sha256 \
  -extfile server.ext 2>/dev/null
echo "$WANT" > server.san
rm -f server.csr server.ext
echo "made a certificate for $WANT"
