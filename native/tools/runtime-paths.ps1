# Shared by the source checkout and the portable Windows package.
function Get-RadaerExecutable {
    $nativeDirectory=Split-Path -Parent $PSScriptRoot
    foreach ($relative in @('build/radaer-native.exe','bin/radaer-native.exe')) {
        $candidate=Join-Path $nativeDirectory $relative
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            # Install a staged build on the next launch, after the old server exits.
            $pending=Join-Path (Split-Path -Parent $candidate) 'radaer-native.pending.exe'
            if (Test-Path -LiteralPath $pending -PathType Leaf) {
                $active=Get-Process -Name 'radaer-native' -ErrorAction SilentlyContinue |
                    Where-Object { $_.Path -eq $candidate }
                if ($active) { throw 'An update is ready. Close the running Radaer server, then launch again.' }
                $backup=Join-Path (Split-Path -Parent $candidate) 'radaer-native.previous.exe'
                [IO.File]::Replace($pending,$candidate,$backup)
            }
            return $candidate
        }
    }
    throw 'radaer-native.exe is missing. Extract the complete portable ZIP or build native/ first.'
}
function Initialize-RadaerRuntime([string]$Executable) {
    $directory=Split-Path -Parent ([IO.Path]::GetFullPath($Executable))
    # Only development builds may use a local toolchain fallback. Portable builds use bundled DLLs.
    if ((Split-Path -Leaf $directory) -eq 'build' -and (Test-Path -LiteralPath 'C:/msys64/ucrt64/bin')) {
        $env:PATH='C:/msys64/ucrt64/bin;'+$env:PATH
    }
    $env:PATH=$directory+';'+$env:PATH
}
