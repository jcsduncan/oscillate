# Install

Oscillate runs on Ubuntu 26.04 x86-64. Each release ships a `.deb` and an
AppImage ([release packages](dependencies/release-packages.md)).

## From the apt repository

The `.deb` of the three newest releases is served from a signed apt
repository at `https://jcsduncan.github.io/oscillate/apt`. Add its key and
source once, then install:

```sh
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://jcsduncan.github.io/oscillate/apt/oscillate-archive-keyring.gpg \
  | sudo tee /etc/apt/keyrings/oscillate-archive-keyring.gpg > /dev/null
echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/oscillate-archive-keyring.gpg] https://jcsduncan.github.io/oscillate/apt resolute main" \
  | sudo tee /etc/apt/sources.list.d/oscillate.list
sudo apt update
sudo apt install oscillate
```

`sudo apt upgrade` then brings each new release. The repository signs its
`InRelease` and `Release.gpg` with the Oscillate archive key; the same key is
published ASCII-armoured as
[`oscillate-archive-keyring.asc`](https://jcsduncan.github.io/oscillate/apt/oscillate-archive-keyring.asc),
with its fingerprint in
[`oscillate-archive-keyring.fingerprint`](https://jcsduncan.github.io/oscillate/apt/oscillate-archive-keyring.fingerprint).
`gpg --show-keys /etc/apt/keyrings/oscillate-archive-keyring.gpg` prints the
fingerprint of the key you installed.

To remove Oscillate and the repository:

```sh
sudo apt remove oscillate
sudo rm /etc/apt/sources.list.d/oscillate.list /etc/apt/keyrings/oscillate-archive-keyring.gpg
```

## AppImage

Download `Oscillate-<version>-x86_64.AppImage` from the
[releases](https://github.com/jcsduncan/oscillate/releases), make it
executable and run it:

```sh
chmod +x Oscillate-*-x86_64.AppImage
./Oscillate-*-x86_64.AppImage
```

## How the repository is published

The [Pages workflow](https://github.com/jcsduncan/oscillate/blob/main/.github/workflows/pages.yml)
deploys the documentation site and the repository together, under `/apt/`,
so neither deployment drops the other. It downloads the `.deb` of the newest
`APT_KEEP_VERSIONS` releases, checks each against its release's
`SHA256SUMS`, builds the `resolute` distribution with aptly
([`build-apt-repository.sh`](https://github.com/jcsduncan/oscillate/blob/main/scripts/ci/build-apt-repository.sh))
and signs it with the key in the `APT_GPG_PRIVATE_KEY` secret, unlocked by
`APT_GPG_PASSPHRASE` when the key has a passphrase. Without the key secret
the site is deployed without `/apt/` and the run shows a notice. The packaging
workflow starts a Pages run once a release's packages are attached, and a
final job follows the instructions above in a clean `ubuntu:26.04` container.

The archive key is a dedicated ed25519 signing key that expires after two
years; the next Pages run after the secret changes publishes the new or
extended key. Installed systems keep the keyring they downloaded, so after a
key change run the `curl` line above again before `sudo apt update`.
