param(
    [switch]$SkipTests,
    # Optional, personal builds only: a MiniFreak .mnfx export inside this
    # folder (for example private\MyBank.mnfx). See docs/MINIFREAK_IMPORT.md.
    [string]$MiniFreakBank = ""
)

# Native Docker commands may emit a startup warning on stderr; handle their
# exit codes explicitly instead of letting PowerShell convert stderr to a
# terminating error.
$ErrorActionPreference = "Continue"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Image = "bizar-builder"
$Mount = "type=bind,source=$Root,target=/work"
$BankEnv = @()
$Tarball = "dist/bizar-module.tar.gz"
if ($MiniFreakBank -ne "") {
    $BankPath = (Resolve-Path $MiniFreakBank).Path
    if (-not $BankPath.StartsWith($Root)) { throw "The MiniFreak bank must be inside $Root so Docker can see it." }
    $Relative = $BankPath.Substring($Root.Length + 1).Replace("\", "/")
    $BankEnv = @("--env", "MINIFREAK_BANK=$Relative")
    $Tarball = "dist/bizar-module-personal.tar.gz"
}

docker info *> $null
if ($LASTEXITCODE -ne 0) {
    throw "Docker Desktop's Linux engine is not running. Start Docker Desktop, wait until it says Engine running, then rerun: powershell -ExecutionPolicy Bypass -File scripts/build-windows.ps1"
}

docker build --file (Join-Path $Root "scripts/Dockerfile") --tag $Image $Root
if ($LASTEXITCODE -ne 0) { throw "Docker image build failed." }

if (-not $SkipTests) {
    docker run --rm --mount $Mount --workdir /work @BankEnv $Image sh scripts/build.sh --test
    if ($LASTEXITCODE -ne 0) { throw "Native plugin tests failed." }
}

docker run --rm --mount $Mount --workdir /work $Image sh scripts/build.sh --reference
if ($LASTEXITCODE -ne 0) { throw "Parameter reference generation failed." }

docker run --rm --mount $Mount --workdir /work @BankEnv $Image sh scripts/build.sh
if ($LASTEXITCODE -ne 0) { throw "ARM64 module build failed." }

$Verify = "file dist/bizar/dsp.so && aarch64-linux-gnu-readelf -h dist/bizar/dsp.so | grep -q 'Machine:.*AArch64' && tar -tzf $Tarball"
docker run --rm --mount $Mount --workdir /work $Image sh -c $Verify
if ($LASTEXITCODE -ne 0) { throw "ARM64 artifact verification failed." }

Write-Host "Build complete: $Root\$($Tarball.Replace('/', '\'))"
