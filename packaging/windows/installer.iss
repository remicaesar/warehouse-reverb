; Warehouse -- Windows VST3 installer (Inno Setup 6).
;
; Plugins only: no Standalone app, no AU (there is no AU on Windows), no
; codesigning step -- this build is unsigned by design, matching the macOS
; .pkg. Ships exactly one component: the VST3, into the canonical 64-bit
; Common Files location Steinberg's spec and Image-Line's manual both point
; to (see packaging assignment notes).
;
; Invoked by .github/workflows/release.yml as:
;   iscc /O<outdir> /F<name> /DBuildDir=<abs build dir> /DAppVersion=<version> \
;        packaging\windows\installer.iss
; /O and /F always win regardless of OutputDir/OutputBaseFilename below, so
; the values here only matter when this script is run by hand.
;
; #ifndef fallbacks below let it be double-clicked / run locally without any
; command-line defines, using the same relative build layout scripts/build.sh
; produces at the repo root.
#ifndef BuildDir
  #define BuildDir "..\..\build"
#endif

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif

[Setup]
AppId={{EC05FEC6-992E-42DC-AAED-E2ACF0CD67ED}
AppName=Warehouse
AppVersion={#AppVersion}
AppPublisher=Emir Sezer
; DefaultDirName is a required [Setup] directive even though CreateAppDir=no
; below means it is never actually used to create a folder or shown on a
; wizard page -- Inno's own docs list DefaultDirName as required regardless.
DefaultDirName={autopf}\Warehouse
; No [Files] entry targets {app} (everything goes to {commoncf64}\VST3), so
; there is nothing to put in a Program Files\Warehouse folder. CreateAppDir=no
; skips creating that empty directory and skips the "Select Destination
; Location" wizard page entirely (verified: jrsoftware.org/ishelp docs state
; this is exactly what CreateAppDir=no does, and that {app} becomes an alias
; for {win} once it is set -- harmless here since nothing installs to {app}).
; One documented side effect: with CreateAppDir=no, uninstall data
; (unins000.exe et al.) is written to the Windows directory instead of an
; app folder; the Add/Remove Programs entry itself still works normally.
CreateAppDir=no
DisableProgramGroupPage=yes
WizardStyle=modern
; Program Files (via {commoncf64}) requires elevation.
PrivilegesRequired=admin
; 64-bit only. x64compatible (not the older, now-deprecated x64/x64os) also
; matches Arm64 Windows 11 running x64 under emulation, which is the
; JR Software-recommended identifier for a plain x64 binary going forward.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\..\LICENSE
OutputDir=..\..\dist
OutputBaseFilename=Warehouse-{#AppVersion}-Setup
Compression=lzma2
SolidCompression=yes

[Files]
; JUCE emits the Windows VST3 as a folder-style bundle, not a single DLL --
; verified against libs/JUCE/extras/Build/CMake/JUCEUtils.cmake
; (_juce_set_plugin_target_properties / _juce_create_windows_package): the
; actual binary lands at
;   Warehouse.vst3\Contents\x86_64-win\Warehouse.vst3
; alongside
;   Warehouse.vst3\Contents\Resources\moduleinfo.json
; so the whole tree must be recursed, not just one file.
Source: "{#BuildDir}\Warehouse_artefacts\Release\VST3\Warehouse.vst3\*"; DestDir: "{commoncf64}\VST3\Warehouse.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
; Inno's own uninstaller only removes the files/folders it created; it does
; not guarantee the now-empty Warehouse.vst3 folder tree goes with them. A
; stale empty bundle folder left behind in the VST3 scan path is exactly the
; kind of thing that makes a DAW report a broken/corrupt plug-in, so remove
; the whole bundle explicitly.
Type: filesandordirs; Name: "{commoncf64}\VST3\Warehouse.vst3"
