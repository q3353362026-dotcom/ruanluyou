$ErrorActionPreference = 'Stop'
$nativeRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'runtime-paths.ps1')
$nativeExe = Get-RadaerExecutable
Initialize-RadaerRuntime $nativeExe
& $nativeExe @args
exit $LASTEXITCODE
