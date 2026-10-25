param(
    [string]$Distro = 'Radiance-Ubuntu',
    [int]$Port = 18790,
    [string]$DataDirectory = '',
    [string]$BuildDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$url = "http://127.0.0.1:$Port"
try {
    $catalog = Invoke-RestMethod -Uri "$url/api/scenarios" -TimeoutSec 2
    if ($null -ne $catalog.scenarios) {
        Write-Host "Romeo is already running: $url"
        return
    }
} catch {}
Push-Location $PSScriptRoot
try {
    & npm.cmd ci
    if ($LASTEXITCODE -ne 0) { throw 'Installing UI dependencies failed. Close any running Vite dev server and retry if its files are in use.' }
    & npm.cmd run build
    if ($LASTEXITCODE -ne 0) { throw 'Building the UI failed.' }
} finally { Pop-Location }
$linuxRepoOutput = & wsl.exe -d $Distro --exec wslpath -a $repo.Replace('\', '/')
if ($LASTEXITCODE -ne 0 -or -not $linuxRepoOutput) { throw 'The WSL repository path could not be resolved.' }
$linuxRepo = $linuxRepoOutput.Trim()
$logDirectory = Join-Path $repo 'runs\launcher'
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$launcherArguments = @('-d', $Distro, '--exec', 'env', "ROMEO_PORT=$Port")
if ($DataDirectory) { $launcherArguments += ('"ROMEO_DATA=' + $DataDirectory + '"') }
if ($BuildDirectory) { $launcherArguments += ('"ROMEO_BUILD_DIR=' + $BuildDirectory + '"') }
$launcherArguments += @('bash', ('"' + $linuxRepo + '/ui/start-local.sh"'))
$process = Start-Process -FilePath 'wsl.exe' -ArgumentList $launcherArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $logDirectory 'stdout.log') -RedirectStandardError (Join-Path $logDirectory 'stderr.log')
$process.Id | Set-Content -LiteralPath (Join-Path $logDirectory 'wsl-process.pid')
Write-Host "Building and starting Romeo in WSL. Open $url when ready."
Write-Host "Startup logs: $logDirectory"
