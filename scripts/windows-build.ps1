# Builds the game on Windows 10/11 with portable tools: nothing is installed,
# no administrator rights. Start it with "Build Daytona.bat" (double-click) in
# the project folder, or:
#
#   powershell -ExecutionPolicy Bypass -File scripts\windows-build.ps1 [-Update] [-NoShortcut]
#
# 1. Downloads into .tools\ (git-ignored), each file checked against its
#    pinned SHA-256: llvm-mingw (Clang, lld, the MinGW-w64 headers with
#    Direct3D 12), CMake, Ninja, Python (embeddable) and MinGit. Later runs
#    reuse them.
# 2. Asks for your ROM set (Revision A, MAME's daytona) the first time and
#    copies it to roms\daytona.zip (or .7z).
# 3. Fetches the pinned libraries (scripts\setup.py), recompiles the game's
#    code from your ROM set and builds it in build-daytona\
#    (scripts\recompile.py). 10 to 40 minutes the first time.
# 4. Puts a "Daytona (recomp)" shortcut on the desktop.
#
# -Update: in a git clone, fetches the latest source first (git pull).
# Everything is logged to windows-build.log. The build holds code derived from
# your ROM set: it is for you only, never share it (rules.md).
param([switch]$Update, [switch]$NoShortcut)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is many times slower with its progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$root = Split-Path -Parent $PSScriptRoot
Set-Location -Path $root
$toolsDir = Join-Path $root '.tools'
$downloads = Join-Path $toolsDir 'downloads'
$buildDir = 'build-daytona'
Start-Transcript -Path (Join-Path $root 'windows-build.log') -Append | Out-Null

function Say([string]$text) { Write-Host ''; Write-Host "== $text" -ForegroundColor Cyan }

# A program run so that its output reaches windows-build.log: Windows
# PowerShell 5.1's transcript does not record what programs write to the
# console themselves, so the compiler's errors were only on screen. cmd merges
# the program's errors into its output (no line becomes a PowerShell error
# under 'Stop'); Python prints as it goes. $LASTEXITCODE is the program's.
function Run-Logged([string]$command) {
    $env:PYTHONUNBUFFERED = '1'
    & cmd /c "$command 2>&1" | Out-Host
}

# The tools, pinned (update URL and SHA-256 together). Inner: the folder the
# archive holds them in, if any. Bin: where the executables are.
$tools = @(
    @{ Name = 'llvm-mingw 20261117 (Clang for Windows)'; Dir = 'llvm-mingw'; Bin = 'bin'
       Url = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/llvm-mingw-20261117-ucrt-x86_64.zip'
       Sha = '4d002e330a0f703625aa5ccbc37e96f8f824413e163bfc7336774aabd2c3a412'; Inner = 'llvm-mingw-20261117-ucrt-x86_64' },
    @{ Name = 'CMake 4.5.1'; Dir = 'cmake'; Bin = 'bin'
       Url = 'https://github.com/Kitware/CMake/releases/download/v4.5.1/cmake-4.5.1-windows-x86_64.zip'
       Sha = 'bfe6eadb245dc8213e0d3feea316b520cb2aae92f5667ffd76059ba6b05c430c'; Inner = 'cmake-4.5.1-windows-x86_64' },
    @{ Name = 'Ninja 1.13.2'; Dir = 'ninja'; Bin = ''
       Url = 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip'
       Sha = '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65'; Inner = '' },
    @{ Name = 'Python 3.14.10 (embeddable)'; Dir = 'python'; Bin = ''
       Url = 'https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip'
       Sha = '90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159'; Inner = '' },
    @{ Name = 'MinGit 2.56.1'; Dir = 'git'; Bin = 'cmd'
       Url = 'https://github.com/git-for-windows/git/releases/download/v2.56.1.windows.1/MinGit-2.56.1-64-bit.zip'
       Sha = '4a92850b6e917da4fd010669b614b55dccf5b0ec333a534cc4be4700dfefdafd'; Inner = '' }
)

# A download, retried: Windows PowerShell 5.1's Invoke-WebRequest can fail
# mid-way on a large file ("the decryption operation failed", a connection
# dropped or inspected by an antivirus), so it is tried three times, then
# with Windows' own curl.exe (Windows 10 1803 and later), which resumes and
# retries by itself. The caller checks the file's SHA-256 either way.
function Get-Download([string]$url, [string]$out) {
    for ($try = 1; $try -le 3; $try++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $out
            return
        } catch {
            Write-Host "     try $try failed: $($_.Exception.Message)"
            if (Test-Path $out) { Remove-Item $out -Force }
            Start-Sleep -Seconds (2 * $try)
        }
    }
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    if (-not (Test-Path $curl)) { throw "Downloading $url failed three times, and this Windows has no curl.exe." }
    Write-Host '     trying with curl.exe...'
    & $curl --fail --location --retry 5 --retry-delay 3 --retry-all-errors --continue-at - --output $out $url
    if ($LASTEXITCODE -ne 0) { throw "Downloading $url failed (Invoke-WebRequest three times, then curl.exe exit $LASTEXITCODE): check the connection, or an antivirus or proxy inspecting HTTPS." }
}

function Get-Tool($t) {
    $dest = Join-Path $toolsDir $t.Dir
    $stamp = Join-Path $dest '.sha256'
    if ((Test-Path $stamp) -and ((Get-Content $stamp -Raw).Trim() -eq $t.Sha)) {
        Write-Host "   $($t.Name): ready"
        return
    }
    New-Item -ItemType Directory -Path $downloads -Force | Out-Null
    $zip = Join-Path $downloads ([IO.Path]::GetFileName($t.Url))
    $good = (Test-Path $zip) -and ((Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower() -eq $t.Sha)
    if (-not $good) {
        Write-Host "   $($t.Name): downloading..."
        Get-Download $t.Url $zip
        $hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
        if ($hash -ne $t.Sha) {
            Remove-Item $zip -Force
            throw "$($t.Name): the download's SHA-256 is $hash, expected $($t.Sha). Nothing was installed; run the build again."
        }
    }
    Write-Host "   $($t.Name): unpacking..."
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    $unpack = Join-Path $toolsDir "unpack-$($t.Dir)"
    if (Test-Path $unpack) { Remove-Item $unpack -Recurse -Force }
    New-Item -ItemType Directory -Path $unpack -Force | Out-Null
    & tar.exe -xf $zip -C $unpack # Windows 10 1803 and later; much faster than Expand-Archive
    if ($LASTEXITCODE -ne 0) { Expand-Archive -Path $zip -DestinationPath $unpack -Force }
    $from = if ($t.Inner) { Join-Path $unpack $t.Inner } else { $unpack }
    Move-Item -Path $from -Destination $dest
    if (Test-Path $unpack) { Remove-Item $unpack -Recurse -Force }
    Set-Content -Path $stamp -Value $t.Sha -Encoding ASCII
}

try {
    Say 'Tools (portable, in .tools\)'
    foreach ($t in $tools) { Get-Tool $t }
    $paths = foreach ($t in $tools) { if ($t.Bin) { Join-Path (Join-Path $toolsDir $t.Dir) $t.Bin } else { Join-Path $toolsDir $t.Dir } }
    $env:PATH = (($paths -join ';') + ';' + $env:PATH)
    $env:CC = 'x86_64-w64-mingw32-clang'
    $env:CXX = 'x86_64-w64-mingw32-clang++'
    $env:M2_COMPILER = 'mingw' # scripts\setup.py: Ninja and these compilers, no Visual Studio

    if ($Update) {
        Say 'Fetching the latest source'
        if (Test-Path (Join-Path $root '.git')) {
            Run-Logged 'git pull --ff-only'
            if ($LASTEXITCODE -ne 0) { throw 'git pull failed (local changes?). The build was not started.' }
        } else {
            Write-Host 'This folder is not a git clone: download the new release instead.'
        }
    }

    Say 'Your ROM set'
    $roms = Join-Path $root 'roms'
    New-Item -ItemType Directory -Path $roms -Force | Out-Null
    $have = @(@('daytona.zip', 'daytona.7z') | Where-Object { Test-Path (Join-Path $roms $_) }) # an array even for one
    if (-not $have) {
        Write-Host 'Choose your Daytona USA ROM set: Revision A, 1994 (MAME''s "daytona", a .zip or .7z).'
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.OpenFileDialog
        $dialog.Title = 'Daytona USA ROM set (Revision A, MAME daytona)'
        $dialog.Filter = 'ROM set (*.zip;*.7z)|*.zip;*.7z'
        if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { throw 'No ROM set chosen: the game cannot be built without it.' }
        $ext = [IO.Path]::GetExtension($dialog.FileName).ToLower()
        Copy-Item -Path $dialog.FileName -Destination (Join-Path $roms ('daytona' + $ext))
        Write-Host "   copied to roms\daytona$ext"
    } else {
        Write-Host "   roms\$($have[0])"
    }

    Say 'Libraries (pinned, into extern\)'
    Run-Logged 'python scripts\setup.py --no-build'
    if ($LASTEXITCODE -ne 0) { throw 'Fetching the libraries failed (see above).' }

    Say 'Recompiling the game from your ROM set and building it (10 to 40 minutes the first time)'
    if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
        Run-Logged "cmake -S . -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DM2_ROMSET=daytona"
        if ($LASTEXITCODE -ne 0) { throw 'CMake could not configure the build (see above).' }
    }
    Run-Logged "python scripts\recompile.py --set daytona --build-dir $buildDir"
    if ($LASTEXITCODE -eq 3) { throw 'Your ROM set was rejected: the "m2import:" line above names the file that is missing or wrong. It must be MAME''s "daytona" (Revision A, 1994).' }
    if ($LASTEXITCODE -ne 0) { throw 'The build failed (the errors are above, and in windows-build.log).' }

    $exe = Join-Path $root "$buildDir\daytona.exe"
    if (-not (Test-Path $exe)) { throw "The build finished without $exe." }
    if (-not $NoShortcut) {
        $link = Join-Path ([Environment]::GetFolderPath('Desktop')) 'Daytona (recomp).lnk'
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($link)
        $shortcut.TargetPath = $exe
        $shortcut.WorkingDirectory = Split-Path -Parent $exe
        $shortcut.Save()
    }
    Say 'Done'
    Write-Host "The game: $exe"
    if (-not $NoShortcut) { Write-Host 'A "Daytona (recomp)" shortcut is on your desktop.' }
    Write-Host 'To update later: run "Build Daytona.bat" again (with -Update in a git clone).'
} catch {
    Write-Host ''
    Write-Host "Build stopped: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host 'The whole log is in windows-build.log.'
    Stop-Transcript | Out-Null
    exit 1
}
Stop-Transcript | Out-Null
