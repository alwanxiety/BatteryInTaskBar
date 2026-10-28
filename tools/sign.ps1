param(
    [string]$Dll = "",
    [string]$Exe = "",
    [string]$Thumbprint = "",
    [switch]$CreateSelfSigned,
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Dll)) { $Dll = Join-Path $root "cmake-build-release\taskbar_mod.dll" }
if ([string]::IsNullOrWhiteSpace($Exe)) { $Exe = Join-Path $root "cmake-build-release\injector.exe" }

$signtool = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if (-not $signtool) {
    throw "signtool.exe not found. Install Windows SDK Signing Tools."
}

if ([string]::IsNullOrWhiteSpace($Thumbprint)) {
    $cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1
    if (-not $cert -and $CreateSelfSigned) {
        Write-Output "Creating a self-signed code-signing certificate..."
        $cert = New-SelfSignedCertificate `
            -Type CodeSigningCert `
            -Subject "CN=BatteryInTaskBar (self-signed)" `
            -CertStoreLocation Cert:\CurrentUser\My `
            -KeyExportPolicy Exportable `
            -KeyUsage DigitalSignature `
            -NotAfter (Get-Date).AddYears(5)
    }
    if (-not $cert) {
        throw "No code-signing certificate. Run with -CreateSelfSigned or pass -Thumbprint."
    }
    $Thumbprint = $cert.Thumbprint
}

$cert = Get-Item "Cert:\CurrentUser\My\$Thumbprint" -ErrorAction SilentlyContinue
if ($cert) {
    Write-Output ("Certificate: {0}" -f $cert.Subject)
    Write-Output ("Thumbprint:  {0}" -f $cert.Thumbprint)
    Write-Output ("Valid until: {0:yyyy-MM-dd}" -f $cert.NotAfter)
}

$files = @($Dll, $Exe) | Where-Object { Test-Path $_ }
if ($files.Count -eq 0) { throw "Nothing to sign: files not found." }

foreach ($file in $files) {
    Write-Output ("Signing: {0}" -f $file)
    $signArgs = @("sign", "/fd", "SHA256", "/sha1", $Thumbprint)
    if ($TimestampUrl) { $signArgs += @("/td", "SHA256", "/tr", $TimestampUrl) }
    $signArgs += $file
    & $signtool.FullName @signArgs | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Write-Output "Timestamp server unavailable, signing without timestamp."
        & $signtool.FullName sign /fd SHA256 /sha1 $Thumbprint $file | Out-Null
        if ($LASTEXITCODE -ne 0) { throw ("Failed to sign {0}. Close the widget and retry." -f $file) }
    }

    $sig = Get-AuthenticodeSignature -FilePath $file
    $statusText = $sig.Status
    if ($sig.Status -eq 'UnknownError' -and $sig.SignerCertificate) {
        $statusText = "signed (self-signed root is not trusted)"
    }
    Write-Output ("  status: {0}" -f $statusText)
    Write-Output ("  signer: {0}" -f $sig.SignerCertificate.Subject)
    if ($sig.Status -ne 'Valid' -and $sig.Status -ne 'UnknownError') {
        Write-Output "  (self-signed root is not trusted by other PCs; use a real certificate for distribution)"
    }
}

Write-Output "Done."
