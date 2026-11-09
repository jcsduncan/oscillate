# Install

Oscillate runs on Ubuntu 26.04 x86-64. Each release ships a `.deb`, an
AppImage ([release packages](dependencies/release-packages.md)) and a
Flatpak bundle for any x86-64 distribution with Flatpak
([Flatpak](dependencies/flatpak.md)).

## From the apt repository

The `.deb` of the three newest releases is served from a signed apt
repository at `https://jcsduncan.github.io/oscillate/apt`. Add its key and
source once, then install:

```sh
sudo apt install curl
curl -fsSLo /tmp/oscillate-archive-keyring.gpg \
  https://jcsduncan.github.io/oscillate/apt/oscillate-archive-keyring.gpg
sudo install -D -m 0644 /tmp/oscillate-archive-keyring.gpg /etc/apt/keyrings/oscillate-archive-keyring.gpg
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
chmod +x Oscillate-<version>-x86_64.AppImage
./Oscillate-<version>-x86_64.AppImage
```

## Flatpak

Download `Oscillate-<version>-x86_64.flatpak` from the
[releases](https://github.com/jcsduncan/oscillate/releases) and install it;
Flatpak fetches the KDE runtime from Flathub:

```sh
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user Oscillate-<version>-x86_64.flatpak
flatpak run io.github.jcsduncan.Oscillate
```

Projects are in `~/.oscillate/projects`, as with the other packages. The
sandbox's permissions and the folder for sampled instrument libraries are in
[Flatpak](dependencies/flatpak.md). To remove it:
`flatpak uninstall --user --delete-data io.github.jcsduncan.Oscillate`.

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
key change run the `curl` and `install` lines above again before
`sudo apt update`.
