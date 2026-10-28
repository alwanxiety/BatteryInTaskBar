# Code signing with SignPath Foundation

This project uses [SignPath Foundation](https://signpath.org/) for free
code-signing of open-source releases. The signing happens in GitHub Actions;
no private key is stored in the repository.

> The local `tools/sign.ps1` script creates a **self-signed** certificate for
> local testing only. It does not remove SmartScreen warnings on other PCs.
> Do not use it for public releases.

## 1. Publish the repository

SignPath Foundation only signs open-source projects. The repository must be
public and contain an OSI-approved license. This repository uses MIT; see
[LICENSE](../LICENSE).

Create the repository on GitHub and push:

```powershell
git remote add origin https://github.com/<your-account>/BatteryInTaskBar.git
git push -u origin main
```

Wait until the `build` workflow finishes successfully. Fix any build errors
before applying for signing.

## 2. Apply to SignPath Foundation

Open <https://signpath.org/apply> and submit the form. Use the repository URL,
the MIT license and the text below (adjust the URL and contact details).

```text
Project: BatteryInTaskBar
Repository: https://github.com/<your-account>/BatteryInTaskBar
License: MIT
Language: C++ (CMake + Visual Studio 2022, Windows SDK 10)
Artifacts: taskbar_mod.dll and injector.exe, built by GitHub Actions on
windows-latest from a tagged release.

Description:
BatteryInTaskBar is an open-source Windows 11 taskbar widget that displays the
battery level of wireless mice and AirPods/Beats. The injector loads a small
DLL into explorer.exe; the DLL draws two taskbar overlay windows and reads
battery information from HID reports, PnP properties and Bluetooth LE
advertisements. There is no telemetry and no network access.

Why signing is required:
The injector is a normal executable downloaded from GitHub Releases. Without a
trusted Authenticode signature SmartScreen shows "Windows protected your PC"
and antivirus products are more likely to flag the DLL injection. A trusted
signature provides a verifiable link between the published binary and the
public repository.

Build command:
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel

Additional notes:
The project is a hobby/utility tool, not affiliated with Microsoft or any
hardware vendor. The source is small and fully open.
```

## 3. Configure SignPath

After approval:

1. Install the [SignPath GitHub App](https://github.com/apps/signpath) and give
   it access to the repository.
2. In SignPath, add the **GitHub.com** trusted build system to the organization
   and link it to the project.
3. Create a project named `BatteryInTaskBar`; the slug will be used by CI
   (for example `battery-in-taskbar`).
4. Create a signing policy with the slug `release`.
5. Create an artifact configuration using
   [signpath-artifact-config.xml](signpath-artifact-config.xml), and mark it as
   the project's default configuration. It expects a ZIP with
   `taskbar_mod.dll` and `injector.exe` at the root.
6. Create an API token for a user with submitter permissions in this project
   and policy.

The default configuration is used by the workflow, so no artifact-configuration
slug is required in the YAML. If you create several configurations, set
`artifact-configuration-slug` in the workflow.

## 4. GitHub repository settings

Add one secret:

| Type | Name | Value |
|---|---|---|
| Secret | `SIGNPATH_API_TOKEN` | API token from SignPath |

Add these repository variables (Settings -> Secrets and variables -> Actions ->
Variables):

| Type | Name | Value |
|---|---|---|
| Variable | `SIGNPATH_ORGANIZATION_ID` | SignPath organization ID |
| Variable | `SIGNPATH_PROJECT_SLUG` | Project slug, e.g. `battery-in-taskbar` |
| Variable | `SIGNPATH_SIGNING_POLICY_SLUG` | `release` |

The workflow uses the default branch's variables; make sure they are set on the
repository, not on a fork.

## 5. Create a release

Bump `APP_VERSION_*` in `res/app_version.h`, commit, and push a tag:

```powershell
git tag v1.0.0
git push origin v1.0.0
```

The `release` workflow will:

1. Check that the tag matches `APP_VERSION_STRING`.
2. Build `taskbar_mod.dll` and `injector.exe` on `windows-latest`.
3. Pack them into a ZIP and upload it as a GitHub Actions artifact.
4. Submit the build to SignPath via `signpath/github-action-submit-signing-request@v3`.
5. Download the signed artifact and verify both signatures with
   `Get-AuthenticodeSignature`.
6. Attach the signed files, a ZIP and `SHA256SUMS.txt` to the GitHub Release.

If the signature check fails, the release is not published.

## 6. After the first signed release

- Keep using the same SignPath project and signing policy for every release.
  Do not replace assets in an existing release.
- Submit the signed `injector.exe` to Microsoft for review:
  <https://www.microsoft.com/en-us/wdsi/filesubmission>
  Choose "I am a software developer" and describe the utility.
- SmartScreen reputation builds over time. SignPath Foundation's certificate is
  trusted, but a brand-new binary may still warn until it gets downloads and
  positive telemetry. A new unsigned build will always warn.
- Do not ask users to install the self-signed certificate. It does not help on
  other PCs and weakens their trust store.

## Notes

- SignPath Foundation signs with its own certificate, so the publisher in the
  file properties will be "SignPath Foundation", not the project author. That
  is expected and is how the service vouches for open-source builds.
- All unsigned build jobs must run on GitHub-hosted runners for OSS signing.
  The workflow uses `windows-latest`.
- The self-signed `tools/sign.ps1` remains useful for local testing
  (`cmake --build build --target sign`), but it is not part of the release
  workflow.
