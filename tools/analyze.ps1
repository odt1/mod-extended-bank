#Requires -Version 7.0
<#
.SYNOPSIS
    Runs every static analyser installed on this machine against one AzerothCore module.

.DESCRIPTION
    Nothing here needs a build. A full worldserver link takes minutes; every check below
    reads the module's own translation units and finishes in under a minute for a
    seven-file module.

    The flags come from the Visual Studio project CMake already generated, so they are the
    real ones -- 298 include directories, 28 preprocessor definitions, /std:c++20 -- and
    they follow the build configuration automatically. They are cached, and regenerated
    whenever the project file is newer than the cache or -Refresh is given.

    Nothing is written inside the module directory. The generated response files live under
    the user's TEMP.

    Analysers, in the order they run:

      codestyle    The module's own apps/ci/ci-codestyle.sh plus the core's
                   apps/codestyle/codestyle-cpp.py, filtered to this module.
      msvc         cl /analyze -- the MSVC static analyser, C6xxx warnings, plus /W4.
      clang-tidy   bugprone, clang-analyzer (path-sensitive), performance, portability.
      cppcheck     A second opinion; shallow here because it does not follow the core
                   headers, but it costs a fifth of a second.

    An analyser whose tool is missing is skipped with a note, never an error.

.PARAMETER ModuleDir
    The module to analyse. Defaults to the parent of the directory holding this script, so
    the script works unchanged when copied into any other module's tools/ folder.

.PARAMETER BuildDir
    The CMake build directory carrying the generated .vcxproj files. Defaults to
    <core>/build.

.PARAMETER Only
    Run only these analysers. Names as listed above.

.PARAMETER Skip
    Run everything except these.

.PARAMETER Jobs
    Parallel clang-tidy invocations. Defaults to the processor count.

.PARAMETER Refresh
    Regenerate the cached compiler flags even if they look current.

.PARAMETER ListTools
    Report which analysers are available and where, then exit.

.PARAMETER Json
    Also write the findings to this path as JSON.

.PARAMETER NoFail
    Always exit 0. Without it the exit code is the number of analysers that found
    something, which is what a CI step or a pre-push hook wants.

.EXAMPLE
    pwsh -File tools/analyze.ps1

.EXAMPLE
    pwsh -File tools/analyze.ps1 -Only clang-tidy,cppcheck

.EXAMPLE
    pwsh -File tools/analyze.ps1 -ModuleDir ..\mod-other -Json findings.json
#>
[CmdletBinding()]
param(
    [string]   $ModuleDir,
    [string]   $BuildDir,
    [string[]] $Only,
    [string[]] $Skip,
    [int]      $Jobs = 0,
    [switch]   $Refresh,
    [switch]   $ListTools,
    [string]   $Json,
    [switch]   $NoFail
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$ALL_ANALYSERS = @('codestyle', 'msvc', 'clang-tidy', 'cppcheck')

# Checks used when the module carries no .clang-tidy of its own. Everything is switched off
# first, then the families worth having are switched back on, then the individual checks
# that are systematically wrong for AzerothCore rather than wrong once:
#
#   performance-unnecessary-value-param  The core passes CharacterDatabaseTransaction by
#                                        value everywhere, and a ChatCommand handler's
#                                        signature is dictated by the argument binder.
#                                        Neither is the module's to change.
#   misc-const-correctness               Fires on most locals in a codebase that does not
#                                        write const on locals.
#   misc-include-cleaner                 Wants every transitively used core header named
#                                        directly; the core's headers are not written that
#                                        way.
#   misc-non-private-member-variables-in-classes
#                                        A module's structs are deliberately plain data.
#   misc-use-internal-linkage            Disagrees with the loader's exported entry point.
#   misc-override-with-different-visibility, bugprone-easily-swappable-parameters,
#   performance-enum-size                Noise at this scale.
#
# A false positive that fires once rather than by category belongs in a NOLINT comment at
# the site, with its reason -- that keeps the check working everywhere else.
$DEFAULT_CLANG_TIDY_CHECKS = @(
    '-*'
    'bugprone-*'
    'clang-analyzer-*'
    'performance-*'
    'portability-*'
    'misc-*'
    'readability-misleading-indentation'
    'readability-inconsistent-declaration-parameter-name'
    'readability-non-const-parameter'
    '-bugprone-easily-swappable-parameters'
    '-misc-const-correctness'
    '-misc-include-cleaner'
    '-misc-non-private-member-variables-in-classes'
    '-misc-override-with-different-visibility'
    '-misc-use-internal-linkage'
    '-performance-enum-size'
    '-performance-unnecessary-value-param'
) -join ','

# cppcheck cannot follow the core headers, so it sees a module source with most of its
# types unknown. What survives that is still worth having, but two style checks fire
# constantly on correct code: functionStatic wants every member function that touches no
# field made static, and useStlAlgorithm rewrites readable loops.
$CPPCHECK_SUPPRESS = @(
    'missingInclude'
    'missingIncludeSystem'
    'functionStatic'
    'useStlAlgorithm'
    'unmatchedSuppression'
    'checkersReport'
)

# --------------------------------------------------------------------------- output

$script:Findings = [System.Collections.Generic.List[object]]::new()

function Write-Head([string]$text) {
    Write-Host ''
    Write-Host "== $text" -ForegroundColor Cyan
}

function Write-Note([string]$text) { Write-Host "   $text" -ForegroundColor DarkGray }
function Write-Good([string]$text) { Write-Host "   $text" -ForegroundColor Green }
function Write-Bad([string]$text) { Write-Host "   $text" -ForegroundColor Yellow }

function Add-Finding([string]$analyser, [string]$line) {
    $script:Findings.Add([pscustomobject]@{ Analyser = $analyser; Text = $line })
}

# --------------------------------------------------------------------------- discovery

function Resolve-ModuleDirectory {
    if ($ModuleDir) { return (Resolve-Path -LiteralPath $ModuleDir).Path }
    return (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
}

# The core root is the first ancestor holding both modules/ and apps/. Walking up beats
# hardcoding "two levels", because a module may be checked out at any depth.
function Resolve-CoreDirectory([string]$moduleDir) {
    $dir = Split-Path -Parent $moduleDir
    while ($dir) {
        if ((Test-Path (Join-Path $dir 'modules')) -and (Test-Path (Join-Path $dir 'apps'))) { return $dir }
        $parent = Split-Path -Parent $dir
        if ($parent -eq $dir) { break }
        $dir = $parent
    }
    return $null
}

# Every module in a stock AzerothCore build compiles into one `modules` target, so that is
# the first guess. The fallback covers a layout where it does not, by finding whichever
# project actually lists one of this module's sources.
function Resolve-ProjectFile([string]$buildDir, [string]$moduleDir) {
    $preferred = Join-Path $buildDir 'modules\modules.vcxproj'
    if (Test-Path $preferred) { return $preferred }

    foreach ($project in Get-ChildItem -Path $buildDir -Filter '*.vcxproj' -Recurse -ErrorAction SilentlyContinue) {
        $text = Get-Content -LiteralPath $project.FullName -Raw -ErrorAction SilentlyContinue
        if ($text -and $text.Contains($moduleDir)) { return $project.FullName }
    }
    return $null
}

function Find-Tool([string]$name, [string[]]$fallbacks) {
    # First match only: `bash` resolves to two of them under a Git for Windows install.
    $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    foreach ($path in $fallbacks) { if (Test-Path $path) { return $path } }
    return $null
}

# `devshell` and friends are pwsh profile functions and do not exist in a non-interactive
# run, so the VS environment is imported directly. A session that already has cl skips it.
function Initialize-VsEnvironment {
    if (Get-Command cl -CommandType Application -ErrorAction SilentlyContinue) { return $true }

    $installerDirectory = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer'
    $vswhere = Join-Path $installerDirectory 'vswhere.exe'
    $install = $null

    # Enter-VsDevShell shells out to vswhere by bare name even when told the install path,
    # so without this the console gets a "'vswhere.exe' is not recognized" line it can do
    # nothing about.
    if ((Test-Path $vswhere) -and ($env:PATH -notlike "*$installerDirectory*")) {
        $env:PATH = "$installerDirectory;$env:PATH"
    }

    if (Test-Path $vswhere) {
        $install = & $vswhere -latest -products '*' `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null | Select-Object -First 1
    }

    if (-not $install) {
        $roots = @('C:\Program Files\Microsoft Visual Studio', 'C:\Program Files (x86)\Microsoft Visual Studio') |
            Where-Object { Test-Path $_ }

        $install = $roots |
            ForEach-Object { Get-ChildItem $_ -Directory -ErrorAction SilentlyContinue } |
            ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } |
            Where-Object { Test-Path (Join-Path $_.FullName 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll') } |
            Select-Object -Last 1 -ExpandProperty FullName
    }

    if (-not $install) { return $false }

    $dll = Join-Path $install 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
    if (-not (Test-Path $dll)) { return $false }

    Import-Module $dll -ErrorAction Stop

    # vswhere is not on PATH and Enter-VsDevShell shells out to it. The resulting complaint
    # on stderr is harmless, because -VsInstallPath already answers what it would ask.
    Enter-VsDevShell -VsInstallPath $install `
        -DevCmdArguments '-arch=x64 -host_arch=x64' -SkipAutomaticLocation 2>$null | Out-Null

    return [bool](Get-Command cl -CommandType Application -ErrorAction SilentlyContinue)
}

# --------------------------------------------------------------------------- flags

# One /D token per definition. A value carrying spaces or quotes -- _CONF_DIR is both -- has
# to survive the response file parser, so the whole token is quoted and inner quotes are
# escaped. Getting this wrong produces "unrecognized source file type 'Files'" out of the
# "Program Files (x86)" in the path, which is a confusing way to learn it.
function Format-Define([string]$definition) {
    if ($definition -match '["\s]') { return '"/D' + $definition.Replace('"', '\"') + '"' }
    return "/D$definition"
}

function Build-ResponseFile([string]$projectFile, [string]$moduleDir, [string]$cacheDir) {
    $msvcResponse = Join-Path $cacheDir 'msvc.rsp'
    $clangResponse = Join-Path $cacheDir 'clang.rsp'

    $current = (Test-Path $msvcResponse) -and (Test-Path $clangResponse) -and
               ((Get-Item $msvcResponse).LastWriteTime -gt (Get-Item $projectFile).LastWriteTime)

    if ($current -and -not $Refresh) {
        Write-Note "flags: cached in $cacheDir"
        return @{ Msvc = $msvcResponse; Clang = $clangResponse }
    }

    $xml = Get-Content -LiteralPath $projectFile -Raw

    $includeMatch = [regex]::Match($xml, '<AdditionalIncludeDirectories>(.*?)</AdditionalIncludeDirectories>', 'Singleline')
    $defineMatch = [regex]::Match($xml, '<PreprocessorDefinitions>(.*?)</PreprocessorDefinitions>', 'Singleline')

    if (-not $includeMatch.Success) {
        throw "No <AdditionalIncludeDirectories> in $projectFile -- has CMake generated it?"
    }

    $includes = @($includeMatch.Groups[1].Value -split ';' | ForEach-Object { $_.Trim() } |
        Where-Object { $_ -and -not $_.StartsWith('%') })

    $defines = @()
    if ($defineMatch.Success) {
        $defines = @($defineMatch.Groups[1].Value -split ';' | ForEach-Object { $_.Trim() } |
            Where-Object { $_ -and -not $_.StartsWith('%') })
    }

    # The module's own headers stay first-class; everything else becomes an external or a
    # system include, so the analysers report the module and stay silent about the core.
    # Without this, cl /analyze alone emits three dozen C6xxx warnings out of fmt and the STL.
    $msvcFlags = @()
    $clangFlags = @()

    foreach ($directory in $includes) {
        if ($directory.StartsWith($moduleDir, [StringComparison]::OrdinalIgnoreCase)) {
            $msvcFlags += "/I`"$directory`""
            $clangFlags += "/I`"$directory`""
        }
        else {
            $msvcFlags += "/external:I`"$directory`""
            $clangFlags += "/imsvc`"$directory`""
        }
    }

    foreach ($definition in $defines) {
        $token = Format-Define $definition
        $msvcFlags += $token
        $clangFlags += $token
    }

    New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null
    Set-Content -LiteralPath $msvcResponse -Value ($msvcFlags -join "`n") -Encoding utf8NoBOM
    Set-Content -LiteralPath $clangResponse -Value ($clangFlags -join "`n") -Encoding utf8NoBOM

    Write-Note "flags: $($includes.Count) include dirs, $($defines.Count) defines, from $(Split-Path -Leaf $projectFile)"
    return @{ Msvc = $msvcResponse; Clang = $clangResponse }
}

# Module sources are globbed at configure time by CollectSourceFiles, so a .cpp added since
# the last `cmake .` is in the module but not in the project. It would be analysed here with
# stale flags and fail at link there. Cheap to notice, expensive to debug.
function Test-ProjectCurrent([string]$projectFile, [object[]]$sources) {
    $xml = Get-Content -LiteralPath $projectFile -Raw
    $missing = @($sources | Where-Object { -not $xml.Contains($_.FullName) })

    if ($missing) {
        Write-Bad "not in $(Split-Path -Leaf $projectFile) -- run 'cmake .' in the build directory:"
        $missing | ForEach-Object { Write-Bad "  $($_.Name)" }
    }
}

# --------------------------------------------------------------------------- analysers

function Invoke-Codestyle([hashtable]$context) {
    Write-Head 'codestyle'
    $dirty = $false

    $moduleScript = Join-Path $context.ModuleDir 'apps\ci\ci-codestyle.sh'
    $bash = Find-Tool 'bash' @('C:\Program Files\Git\bin\bash.exe')

    if ((Test-Path $moduleScript) -and $bash) {
        # It greps a relative "src", so it has to run from the module root, or it silently
        # scans the core's sources instead and reports their tabs as yours.
        Push-Location $context.ModuleDir
        try { $output = & $bash 'apps/ci/ci-codestyle.sh' 2>&1 } finally { Pop-Location }

        if ("$output" -notmatch 'Everything looks good') {
            $output | ForEach-Object { Write-Bad $_; Add-Finding 'codestyle' "$_" }
            $dirty = $true
        }
    }
    elseif (-not $bash) {
        Write-Note 'module CI script skipped: no bash on PATH'
    }

    $coreLint = Join-Path $context.CoreDir 'apps\codestyle\codestyle-cpp.py'
    $python = Find-Tool 'python' @("$env:USERPROFILE\.local\bin\python.exe")

    if ((Test-Path $coreLint) -and $python) {
        Push-Location $context.CoreDir
        try { $output = & $python $coreLint 2>&1 } finally { Pop-Location }

        $mine = @($output | Where-Object { "$_" -like "*$(Split-Path -Leaf $context.ModuleDir)*" })
        if ($mine) {
            $mine | ForEach-Object { Write-Bad $_; Add-Finding 'codestyle' "$_" }
            $dirty = $true
        }
    }
    elseif (-not $python) {
        Write-Note 'core lint skipped: no python on PATH'
    }

    if (-not $dirty) { Write-Good 'clean' }
    return $dirty
}

function Invoke-Msvc([hashtable]$context) {
    Write-Head 'cl /analyze'

    if (-not (Initialize-VsEnvironment)) {
        Write-Note 'skipped: no Visual Studio C++ toolset found'
        return $false
    }

    # /analyze:only      analyse, emit no object files
    # /analyze:autolog-  do not litter the working directory with .nativecodeanalysis.xml
    # /analyze:external- together with /external:W0, the core and the STL are somebody
    #                    else's problem
    $arguments = @(
        '/nologo', '/c', '/utf-8', '/Zc:preprocessor',
        '/analyze:only', '/analyze:autolog-', '/analyze:external-', '/external:W0',
        '/std:c++20', '/EHsc', '/MD', '/W4',
        "@$($context.MsvcResponse)"
    ) + @($context.Sources | ForEach-Object { $_.FullName })

    Push-Location $context.ModuleDir
    try { $output = & cl @arguments 2>&1 } finally { Pop-Location }

    # cl echoes each source file name as it starts it; a real diagnostic carries a code.
    $hits = @($output | Where-Object {
        "$_" -match '(warning|error) [A-Z]+\d+' -and "$_" -like "*$($context.ModuleDir)*"
    })

    if ($hits) {
        $hits | ForEach-Object { Write-Bad $_; Add-Finding 'msvc' "$_" }
        return $true
    }

    Write-Good "clean ($($context.Sources.Count) files)"
    return $false
}

function Invoke-ClangTidy([hashtable]$context) {
    Write-Head 'clang-tidy'

    $tidy = Find-Tool 'clang-tidy' @('C:\Program Files\LLVM\bin\clang-tidy.exe')
    if (-not $tidy) {
        Write-Note 'skipped: clang-tidy not found (install LLVM)'
        return $false
    }

    # A .clang-tidy beside the sources is authoritative and clang-tidy finds it on its own.
    # Passing --checks would override it, so that only happens when there is none.
    $hasConfig = Test-Path (Join-Path $context.ModuleDir '.clang-tidy')
    Write-Note $(if ($hasConfig) { 'using the module .clang-tidy' } else { 'using the built-in check list' })

    $headerFilter = '^' + [regex]::Escape($context.ModuleDir)
    $parallel = if ($Jobs -gt 0) { $Jobs } else { [Environment]::ProcessorCount }
    $checks = $DEFAULT_CLANG_TIDY_CHECKS
    $response = $context.ClangResponse

    $output = $context.Sources | ForEach-Object -ThrottleLimit $parallel -Parallel {
        $arguments = @('--quiet', "--header-filter=$using:headerFilter")
        if (-not $using:hasConfig) { $arguments += "--checks=$using:checks" }
        $arguments += $_.FullName
        $arguments += @('--', '--driver-mode=cl', '/std:c++20', '/EHsc', '/MD', '/utf-8',
            '/Zc:preprocessor', "@$using:response")
        & $using:tidy @arguments 2>&1
    }

    $hits = @($output | Where-Object { "$_" -match ':\d+:\d+: (warning|error):' } | Select-Object -Unique)

    if ($hits) {
        $hits | ForEach-Object { Write-Bad $_; Add-Finding 'clang-tidy' "$_" }
        return $true
    }

    Write-Good "clean ($($context.Sources.Count) files)"
    return $false
}

function Invoke-Cppcheck([hashtable]$context) {
    Write-Head 'cppcheck'

    $cppcheck = Find-Tool 'cppcheck' @(
        'C:\Program Files\Cppcheck\cppcheck.exe',
        'C:\Program Files (x86)\Cppcheck\cppcheck.exe')

    if (-not $cppcheck) {
        Write-Note 'skipped: cppcheck not found'
        return $false
    }

    # Deliberately not given the core's 298 include directories. cppcheck would spend
    # minutes parsing headers it cannot fully model anyway, and the checks that survive
    # unknown types -- the ones worth having here -- do not need them.
    $arguments = @(
        '--quiet', '--enable=warning,style,performance,portability',
        '--std=c++20', '--language=c++', '--platform=win64',
        '--inline-suppr', '--check-level=exhaustive', '-Isrc', 'src'
    )

    foreach ($id in $CPPCHECK_SUPPRESS) { $arguments += "--suppress=$id" }

    Push-Location $context.ModuleDir
    try { $output = & $cppcheck @arguments 2>&1 } finally { Pop-Location }

    $hits = @($output | Where-Object { "$_" -match ':\d+:\d+: (error|warning|style|performance|portability):' })

    if ($hits) {
        $hits | ForEach-Object { Write-Bad $_; Add-Finding 'cppcheck' "$_" }
        return $true
    }

    Write-Good 'clean'
    return $false
}

# --------------------------------------------------------------------------- main

$moduleDirectory = Resolve-ModuleDirectory
$moduleName = Split-Path -Leaf $moduleDirectory
$coreDirectory = Resolve-CoreDirectory $moduleDirectory

if (-not $coreDirectory) {
    throw "No AzerothCore root above $moduleDirectory (looked for a directory holding both modules/ and apps/)."
}

if (-not $BuildDir) { $BuildDir = Join-Path $coreDirectory 'build' }

$sources = @(Get-ChildItem -Path (Join-Path $moduleDirectory 'src') -Filter '*.cpp' -Recurse -ErrorAction SilentlyContinue)
if (-not $sources) { throw "No .cpp files under $moduleDirectory\src." }

if ($ListTools) {
    Write-Head 'tools'
    $cl = if (Initialize-VsEnvironment) { (Get-Command cl).Source } else { 'NOT FOUND' }
    Write-Host ('   {0,-11} {1}' -f 'cl', $cl)

    foreach ($tool in @(
            @{ Name = 'clang-tidy'; Fallbacks = @('C:\Program Files\LLVM\bin\clang-tidy.exe') },
            @{ Name = 'cppcheck'; Fallbacks = @('C:\Program Files\Cppcheck\cppcheck.exe') },
            @{ Name = 'python'; Fallbacks = @("$env:USERPROFILE\.local\bin\python.exe") },
            @{ Name = 'bash'; Fallbacks = @('C:\Program Files\Git\bin\bash.exe') })) {

        $found = Find-Tool $tool.Name $tool.Fallbacks
        Write-Host ('   {0,-11} {1}' -f $tool.Name, $(if ($found) { $found } else { 'NOT FOUND' }))
    }
    exit 0
}

# `pwsh -File` hands every argument over as a plain string, so -Only clang-tidy,cppcheck
# arrives as one element containing a comma rather than as two. Splitting here makes the
# documented invocation behave the same as calling the script from inside a session.
function Expand-NameList([string[]]$values) {
    if (-not $values) { return @() }
    return @($values | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

$onlyNames = Expand-NameList $Only
$skipNames = Expand-NameList $Skip

$unknown = @($onlyNames + $skipNames | Where-Object { $_ -notin $ALL_ANALYSERS })
if ($unknown) { throw "Unknown analyser: $($unknown -join ', '). Known: $($ALL_ANALYSERS -join ', ')." }

$selected = $ALL_ANALYSERS
if ($onlyNames) { $selected = @($ALL_ANALYSERS | Where-Object { $onlyNames -contains $_ }) }
if ($skipNames) { $selected = @($selected | Where-Object { $skipNames -notcontains $_ }) }
if (-not $selected) { throw "No analysers selected. Known: $($ALL_ANALYSERS -join ', ')." }

Write-Head "$moduleName -- $($sources.Count) sources"
Write-Note "core:  $coreDirectory"
Write-Note "build: $BuildDir"

$responses = @{ Msvc = $null; Clang = $null }

# Only the compiler-driven analysers need the project's flags; codestyle and cppcheck do not,
# which is what lets those two run on a checkout that has never been configured.
if (@($selected | Where-Object { $_ -in @('msvc', 'clang-tidy') })) {
    $projectFile = if (Test-Path $BuildDir) { Resolve-ProjectFile $BuildDir $moduleDirectory } else { $null }

    if (-not $projectFile) {
        Write-Bad "No .vcxproj under $BuildDir lists this module's sources -- skipping cl and clang-tidy."
        Write-Bad "Configure CMake once ('cmake .' in the build directory), then re-run."
        $selected = @($selected | Where-Object { $_ -notin @('msvc', 'clang-tidy') })
    }
    else {
        $responses = Build-ResponseFile $projectFile $moduleDirectory (Join-Path $env:TEMP "acore-analyze\$moduleName")
        Test-ProjectCurrent $projectFile $sources
    }
}

$context = @{
    ModuleDir     = $moduleDirectory
    CoreDir       = $coreDirectory
    Sources       = $sources
    MsvcResponse  = $responses.Msvc
    ClangResponse = $responses.Clang
}

$stopwatch = [Diagnostics.Stopwatch]::StartNew()
$dirtyAnalysers = @()

foreach ($analyser in $ALL_ANALYSERS) {
    if ($analyser -notin $selected) { continue }

    $dirty = switch ($analyser) {
        'codestyle' { Invoke-Codestyle $context }
        'msvc' { Invoke-Msvc $context }
        'clang-tidy' { Invoke-ClangTidy $context }
        'cppcheck' { Invoke-Cppcheck $context }
    }

    if ($dirty) { $dirtyAnalysers += $analyser }
}

$stopwatch.Stop()

Write-Head 'summary'
Write-Note "$([math]::Round($stopwatch.Elapsed.TotalSeconds, 1))s, $($selected.Count) analysers, $($script:Findings.Count) findings"

if ($dirtyAnalysers) {
    Write-Bad "findings from: $($dirtyAnalysers -join ', ')"
}
else {
    Write-Good 'all clean'
}

if ($Json) {
    $script:Findings | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $Json -Encoding utf8NoBOM
    Write-Note "wrote $Json"
}

if ($NoFail) { exit 0 }
exit $dirtyAnalysers.Count
