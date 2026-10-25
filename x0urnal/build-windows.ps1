# build-windows.ps1 -- build X0urnal++ for Windows x86_64 from this checkout with MSYS2 (UCRT64),
# install it into %USERPROFILE%\X0urnal++ and put book-sync into %USERPROFILE%\bin.
#
#   pwsh -File x0urnal\build-windows.ps1
#
# Needs MSYS2 (https://www.msys2.org; C:\msys64, or set MSYS2_ROOT) and, for book-sync, Python 3.11+
# with python/pythonw on PATH. Run it with PowerShell 7 (pwsh).
#
# It follows upstream's own recipe: readme/WindowsBuild.md for the toolchain, and
# windows-setup/package.sh for the runtime -- including two things nobody would guess: the
# gdk-pixbuf loaders are found by running `strings` over the cache, and the gspawn helper exes
# must be copied or GLib cannot spawn.
#
# THREE WINDOWS-ONLY FACTS:
#
#   1. THE DARK VARIANT. Windows defaults gtk-theme-name to plain `Adwaita`, so `prefer-dark` alone
#      resolves the dark variant. etc/gtk-3.0/settings.ini is written anyway, so the result does
#      not depend on that default staying put.
#
#   2. CMAKE 4.x TRIPS ON UPSTREAM'S `if (${PACKAGE_ARCH} STREQUAL "x86_64")` when MSYS2 leaves
#      CMAKE_SYSTEM_PROCESSOR empty. -D cannot fix it (it is a non-cache set()), so the
#      environment is what is set (PROCESSOR_ARCHITECTURE below).
#
#   3. etc/fonts MUST BE BUNDLED, which upstream's package.sh does not do. Without it every run
#      prints "Fontconfig error: Cannot load default config file" and the font falls back. Its
#      WINDOWSFONTDIR token finds the theme's font wherever Windows has it installed.
$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Dest = "$env:USERPROFILE\X0urnal++"
$Msys = if ($env:MSYS2_ROOT) { $env:MSYS2_ROOT } else { 'C:\msys64' }
$Build = if ($env:X0URNAL_BUILD) { $env:X0URNAL_BUILD } else { "$Root\build-windows" }
if (-not (Test-Path "$Msys\usr\bin\bash.exe")) { throw "MSYS2 not found in $Msys (set MSYS2_ROOT)" }

function Ucrt([string]$cmd) {
  & "$Msys\usr\bin\bash.exe" -lc $cmd
  if ($LASTEXITCODE -ne 0) { throw "UCRT64 command failed ($LASTEXITCODE): $cmd" }
}
function Unix([string]$p) { (& "$Msys\usr\bin\cygpath.exe" -u $p).Trim() }
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = '1'
# See fact 2 above. This is the whole fix; it is an environment variable, not a patch.
$env:PROCESSOR_ARCHITECTURE = 'AMD64'

Write-Host '== dependencies (UCRT64)'
# From upstream's CMakeLists. Upstream's doc says `portaudio`; CMake also wants portaudiocpp, and
# the same package provides both.
& "$Msys\usr\bin\pacman.exe" -S --needed --noconfirm `
  mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja `
  mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-imagemagick mingw-w64-ucrt-x86_64-gettext `
  mingw-w64-ucrt-x86_64-poppler mingw-w64-ucrt-x86_64-gtk3 mingw-w64-ucrt-x86_64-libsndfile `
  mingw-w64-ucrt-x86_64-libzip mingw-w64-ucrt-x86_64-lua mingw-w64-ucrt-x86_64-portaudio `
  mingw-w64-ucrt-x86_64-gtksourceview4 mingw-w64-ucrt-x86_64-qpdf mingw-w64-ucrt-x86_64-librsvg `
  mingw-w64-ucrt-x86_64-libxml2 mingw-w64-ucrt-x86_64-zlib git | Out-Null

Write-Host '== build'
$r = Unix $Root; $b = Unix $Build; $d = Unix $Dest
Ucrt "cmake -G Ninja -S '$r' -B '$b' -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX='$d' >/dev/null && ninja -C '$b'"
Ucrt "ninja -C '$b' install >/dev/null"

Write-Host '== the GTK runtime: schemas, pixbuf loaders, icons, fonts, helpers'
Ucrt @'
D="$(cygpath -u "$USERPROFILE")/X0urnal++"
P=/ucrt64
mkdir -p "$D/share/glib-2.0/schemas" "$D/lib/gdk-pixbuf-2.0" "$D/etc/gtk-3.0" "$D/etc/fonts"
cp -rn $P/share/glib-2.0/schemas/*.xml "$D/share/glib-2.0/schemas/" 2>/dev/null
glib-compile-schemas "$D/share/glib-2.0/schemas" 2>/dev/null
cp -rn $P/lib/gdk-pixbuf-2.0/* "$D/lib/gdk-pixbuf-2.0/" 2>/dev/null
# share/icons (all of it -- AdwaitaLegacy holds the old icon names GTK3 still asks for), poppler's
# font maps (CJK PDFs), gtksourceview-4 (the LaTeX editor's highlighting) and the whole glib-2.0:
# upstream's windows-setup/package.sh copies all four.
mkdir -p "$D/share/icons"
cp -rn $P/share/icons/* "$D/share/icons/" 2>/dev/null
cp -rn $P/share/poppler $P/share/gtksourceview-4 "$D/share/" 2>/dev/null
cp -rn $P/share/glib-2.0/* "$D/share/glib-2.0/" 2>/dev/null
for need in icons/AdwaitaLegacy poppler/cMap gtksourceview-4/language-specs; do
  [ -d "$D/share/$need" ] || { echo "MISSING share/$need"; exit 1; }
done
cp -rn $P/etc/fonts/* "$D/etc/fonts/" 2>/dev/null        # fact 3: else fontconfig cannot start
cp -n $P/bin/gspawn-win64-helper*.exe $P/bin/gdbus.exe "$D/bin/" 2>/dev/null
# fact 1: pin the dark variant rather than trusting a default to stay put
printf '[Settings]\ngtk-application-prefer-dark-theme=1\ngtk-theme-name=Adwaita\n' > "$D/etc/gtk-3.0/settings.ini"
# the transitive DLL closure, to a fixed point
cd "$D/bin" || exit 1
for pass in 1 2 3 4 5 6; do
  before=$(ls *.dll 2>/dev/null | wc -l)
  for f in *.exe *.dll ../lib/gdk-pixbuf-2.0/*/loaders/*.dll; do
    [ -f "$f" ] && ldd "$f" 2>/dev/null | awk '/ucrt64/ {print $3}'
  done | sort -u | while read -r dll; do
    [ -f "$dll" ] && [ ! -f "./$(basename "$dll")" ] && cp -n "$dll" ./
  done
  after=$(ls *.dll 2>/dev/null | wc -l)
  [ "$before" = "$after" ] && break
done
echo "dlls: $(ls *.dll | wc -l)"
'@

if (-not (Select-String -Quiet -Path "$Dest\share\xournalpp\ui\xournalpp.css" -Pattern 'BigBlueTerm437 Nerd Font Mono')) {
  throw 'ui\xournalpp.css is not the X0urnal++ stylesheet -- was this built from the x0urnal branch?'
}

# bin\X0urnal++.exe -- the name to launch. A copy, not a symlink: a Windows symlink needs
# Developer Mode or elevation. Upstream's xournalpp.exe stays beside it.
Copy-Item "$Dest\bin\xournalpp.exe" "$Dest\bin\X0urnal++.exe" -Force
$stamp = (Get-Item "$Dest\bin\X0urnal++.exe").LastWriteTime
if ($stamp -lt (Get-Date).AddMinutes(-10)) { throw "X0urnal++.exe is stale ($stamp) -- the copy did not take" }

Write-Host '== book-sync -> %USERPROFILE%\bin'
# X0urnal++ looks for book-sync on PATH and in %USERPROFILE%\bin, and runs it through pythonw.
$Bin = "$env:USERPROFILE\bin"
New-Item -ItemType Directory -Force -Path $Bin | Out-Null
foreach ($f in 'book-sync', 'booklib.py', 'xopp-merge', 'notebook-merge', 'xopp-filter', 'book-sync.cmd') {
  Copy-Item "$Root\book-sync\$f" "$Bin\$f" -Force
}
if (-not (Get-Command pythonw -ErrorAction SilentlyContinue)) {
  Write-Warning 'pythonw is not on PATH: install Python 3.11+ (python.org or winget), or books will only save locally'
}

Write-Host "X0urnal++ installed: $Dest  (launch bin\X0urnal++.exe, $stamp)"
