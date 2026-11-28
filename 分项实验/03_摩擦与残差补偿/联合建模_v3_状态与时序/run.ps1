param(
    [ValidateSet('baseline','A','B','C','internal','report','all')]
    [string]$Stage = 'all',
    [string]$Python = 'C:/Users/HYB/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
)
$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $PSScriptRoot
$localDependencies = Join-Path $PSScriptRoot '../../../tmp/zr3_python'
if (Test-Path -LiteralPath $localDependencies) {
    $env:PYTHONPATH = (Resolve-Path -LiteralPath $localDependencies).Path
}
$stages = if ($Stage -eq 'all') { @('baseline','A','B','C','internal','report') } else { @($Stage) }
foreach ($name in $stages) {
    & $Python -X utf8 -B study.py $name
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

