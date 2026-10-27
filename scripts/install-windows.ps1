param(
    [string]$MoveHost = "move.local",
    [string]$SshKeyPath = "",
    # Install the personal build made with -MiniFreakBank instead.
    [switch]$Personal
)

$ErrorActionPreference = "Continue"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ArchiveName = if ($Personal) { "bizar-module-personal.tar.gz" } else { "bizar-module.tar.gz" }
$Archive = Join-Path $Root "dist\$ArchiveName"
$Remote = "root@$MoveHost"
$RemoteDir = "/data/UserData/schwung/modules/sound_generators"
$SshOptions = @()

if ($SshKeyPath) {
    if (-not (Test-Path $SshKeyPath)) { throw "SSH private key not found: $SshKeyPath" }
    $SshOptions += @("-i", $SshKeyPath, "-o", "IdentitiesOnly=yes")
}

if (-not (Test-Path $Archive)) {
    throw "Module archive not found at $Archive. Run scripts/build-windows.ps1 first."
}

Push-Location $Root
try {
    & scp @SshOptions $Archive "${Remote}:/tmp/bizar-module.tar.gz"
    if ($LASTEXITCODE -ne 0) {
        throw "Upload failed. Check that Move is reachable and your Schwung SSH key is configured."
    }

    $InstallCommand = "mkdir -p $RemoteDir && tar -xzf /tmp/bizar-module.tar.gz -C $RemoteDir && chown -R ableton:users $RemoteDir/bizar && rm /tmp/bizar-module.tar.gz"
    & ssh @SshOptions $Remote $InstallCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Remote extraction failed. Archive is still at /tmp/bizar-module.tar.gz on Move."
    }
} finally {
    Pop-Location
}

Write-Host "Installed Bizar to $RemoteDir/bizar on $MoveHost. Rescan modules or restart Schwung to load it."
