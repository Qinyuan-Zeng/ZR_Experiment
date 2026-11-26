$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$folder = Join-Path $root "obj/replay-$PID"
New-Item -ItemType Directory -Path $folder | Out-Null
$header = 'frame,card_tick_raw,encoder_position_raw,current_feedback_raw'

function Write-Frames($name, [long[]]$ticks) {
    $path = Join-Path $folder "$name.csv"
    $rows = for ($i = 0; $i -lt $ticks.Count; $i++) {
        $current = if ($i -lt 2) { -123 } else { 14 }
        "$i,$($ticks[$i]),16000,$current"
    }
    @($header) + $rows | Set-Content -LiteralPath $path -Encoding ascii
    $path
}

function Assert-Replay($exe, $file, $expectedExit, [string[]]$expected) {
    $output = & $exe --replay $file 500000 2>&1 | ForEach-Object { $_.ToString() }
    if ($LASTEXITCODE -ne $expectedExit) { throw "Unexpected exit for $file" }
    foreach ($line in $expected) {
        if ($line -notin $output) { throw "Missing '$line' in replay of $file" }
    }
}

try {
    # Signed 32-bit clock values at rollover, with negative then positive current.
    $regular = Write-Frames regular (-3..97)
    $gapped = Write-Frames gapped @(0..15; 28..100)
    $repeat = Write-Frames repeat @(0; 0; 1..65)
    $backwards = Write-Frames backwards @(10; 9; 11..75)
    $short = Write-Frames short @(0..3)
    $invalid = Join-Path $folder invalid.csv
    @($header, '0,0,16000,65413') | Set-Content -LiteralPath $invalid -Encoding ascii
    foreach ($configuration in 'Debug', 'Release') {
        $exe = Join-Path $root "bin/x64/$configuration/probe_hs_capture/ZRInertiaCurrent.exe"
        Assert-Replay $exe $regular 0 @(
            'frames=101', 'clock_wraps=1', 'gap_intervals=0',
            'frame_interval_median_ms=0.5', 'frame_interval_p95_ms=0.5',
            'frame_interval_max_ms=0.5', 'window_5_ms_frames=10..10',
            'window_15_ms_frames=30..30', 'window_23.3_ms_frames=46..47',
            'equal_current_run_frames:frequency 2:1 99:1',
            'current_feedback_update_interval=unmeasured'
        )
        Assert-Replay $exe $gapped 0 @(
            'gap_intervals=1', 'missing_core_ticks=12', 'frame_interval_max_ms=6.5',
            'window_5_ms_frames=0..10', 'window_15_ms_frames=18..30',
            'window_23.3_ms_frames=34..47', 'equal_current_run_frames:frequency 2:1 14:1 73:1'
        )
        Assert-Replay $exe $repeat 0 @('clock_duplicates=1', 'window_5_ms_frames=10..11')
        Assert-Replay $exe $backwards 1 @('clock_backwards=1', 'window_5_ms_frames=unavailable')
        Assert-Replay $exe $short 0 @('window_23.3_ms_frames=unavailable')
        Assert-Replay $exe $invalid 1 @('Current channel does not contain signed 16-bit raw values')
        Write-Output "$configuration x64 replay PASS"
    }
} finally {
    Get-ChildItem -LiteralPath $folder -File -Filter '*.csv' | ForEach-Object {
        Remove-Item -LiteralPath $_.FullName
    }
    Remove-Item -LiteralPath $folder
}
