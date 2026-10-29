param(
    [Parameter(Mandatory=$true)][string]$BuildDirectory,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-f]{40}$')][string]$SourceRevision,
    [string]$CMakeExecutable = 'cmake',
    [string]$CTestsExecutable = 'ctest'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$sourceDirectory = Split-Path -Parent $PSScriptRoot
& $CMakeExecutable -S $sourceDirectory -B $BuildDirectory -DFETCHCONTENT_FULLY_DISCONNECTED=ON -DENGINE_BUILD_TESTS=ON "-DAUDIOCPP_VERSION=drama3-native-$SourceRevision"
if ($LASTEXITCODE -ne 0) { throw 'Drama3 CMake configuration failed' }
$targets = @('audiocpp_server', 'fish_drama3_reference_input_test', 'fish_drama3_sampler_test',
    'fish_drama3_codec_source_test', 'fish_drama3_expert_quantization_test',
    'text_chunking_test', 'streaming_audio_test', 'server_runtime_selection_test')
& $CMakeExecutable --build $BuildDirectory --parallel 3 --target @targets
if ($LASTEXITCODE -ne 0) { throw 'Drama3 native build failed' }
& $CTestsExecutable --test-dir $BuildDirectory -R '^(fish_drama3_(reference_input|sampler|codec_source|expert_quantization)_test|text_chunking_test|streaming_audio_test|server_runtime_selection_test)$' --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Drama3 native tests failed' }
$binary = Join-Path $BuildDirectory 'bin/audiocpp_server.exe'
$receipt = [ordered]@{schema='audiocpp.drama3.local-ci.v1'; source_revision=$SourceRevision;
    platform='windows-native'; server_sha256=(Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant();
    tests=$targets[1..($targets.Count-1)]; model_inference='not_run'; completed_utc=[DateTime]::UtcNow.ToString('o')}
$receipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $BuildDirectory 'drama3-local-ci.json') -Encoding UTF8
