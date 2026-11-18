# Code signing policy

Free code signing provided by [SignPath.io](https://about.signpath.io), certificate by
[SignPath Foundation](https://signpath.org).

## What is signed

Only the binaries built from this repository by the [GitHub Actions workflow](.github/workflows/build.yml):
`mtsrec.exe` and the `mtsrec-<version>.msi` installer. Third-party files included in the installer keep
their own signatures (the Python embeddable package is signed by the Python Software Foundation).
Every signing request is approved manually before a release.

## Team roles

- Committers and reviewers: [Andrea Angeli](https://github.com/angeliandrea)
- Approvers: [Andrea Angeli](https://github.com/angeliandrea)

All accounts with access to this repository and to SignPath use multi-factor authentication.

## Privacy statement

This program will not transfer any information to other networked systems unless specifically requested
by the user or the person installing or operating it.

In detail: on the local network the recorder talks only to the ATEM switcher and HyperDeck addresses the
user enters, and serves its own page on the same computer. It never connects to the internet by itself.
The test report is created only when the user presses *Test report…*; the user sees its content, it is
saved on the user's Desktop, and the user decides whether to send it, by email or by uploading it to the
developer's Filemail page, which the program opens in the web browser only when the user presses *Open upload page*. It contains no video, no IP addresses, no computer or user names and
no file paths.
