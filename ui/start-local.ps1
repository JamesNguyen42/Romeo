param(
    [string]$Distro = $(if ($env:ROMEO_WSL_DISTRO) { $env:ROMEO_WSL_DISTRO } else { 'Radiance-Ubuntu' }),
    [ValidateRange(1, 65535)]
    [int]$Port = $(if ($env:ROMEO_PORT) { [int]$env:ROMEO_PORT } else { 18790 }),
    [string]$DataDirectory = $env:ROMEO_DATA,
    [string]$BuildDirectory = $env:ROMEO_BUILD_DIR
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Get-Command npm.cmd -ErrorAction Stop | Out-Null
Get-Command wsl.exe -ErrorAction Stop | Out-Null

# Always rebuild first. An existing listener must never make us skip new story markup.
Push-Location $PSScriptRoot
try {
    & npm.cmd run build
    if ($LASTEXITCODE -ne 0) { throw 'Building the story UI failed.' }
} finally { Pop-Location }

function Convert-ToWslPath([string]$Path) {
    $converted = & wsl.exe -d $Distro --exec wslpath -a $Path.Replace('\', '/')
    if ($LASTEXITCODE -ne 0 -or -not $converted) { throw "The WSL path could not be resolved: $Path" }
    return ($converted | Out-String).Trim()
}

$linuxRepo = Convert-ToWslPath $repo
$launcherArguments = @('-d', $Distro, '--exec', 'env', "ROMEO_PORT=$Port")
if ($DataDirectory) {
    if ($DataDirectory -match '^[A-Za-z]:[\\/]') { $DataDirectory = Convert-ToWslPath $DataDirectory }
    $launcherArguments += "ROMEO_DATA=$DataDirectory"
}
if ($BuildDirectory) {
    if ($BuildDirectory -match '^[A-Za-z]:[\\/]') { $BuildDirectory = Convert-ToWslPath $BuildDirectory }
    $launcherArguments += "ROMEO_BUILD_DIR=$BuildDirectory"
}
$launcherArguments += @('bash', "$linuxRepo/ui/start-local.sh", '--ui-built')
Write-Host "Starting Juliet's story UI and API in $Distro. Keep this terminal open; Ctrl+C stops the service."
& wsl.exe @launcherArguments
if ($LASTEXITCODE -ne 0) { throw "Juliet exited with code $LASTEXITCODE. See the message above." }
