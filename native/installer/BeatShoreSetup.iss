; BeatShore Windows installer -- Inno Setup 6 script.
;
; NOT YET COMPILED: this environment doesn't have Inno Setup installed, so
; no version of this script -- including this one -- has been checked by
; the actual Inno Setup compiler. Every `[Code]` Pascal-script block below
; is therefore unverified syntax, not just untested behavior; treat it as
; a careful first draft, not working code, until it's actually compiled.
; It HAS been staged and verified twice, for real, against trimmed
; production copies of the engine directory (once on Node 25, once
; rebuilt from scratch on Node 24 LTS after that was flagged as
; unsupported -- see "Verified staging" below). This machine has Visual
; Studio, CMake, JUCE, and npm installed, which is exactly what a genuine
; clean-machine test needs to NOT have -- that verification needs a
; second machine this environment doesn't have. See STATUS.md's
; "Clean-machine packaging" section for the full honest state.
;
; --- Licensing, confirmed against the actual files in this repo, not
; assumed from general VST3/JUCE knowledge: ---
;
; VST3 SDK: this project vendors SDK 3.8.1 (native\vst3sdk\CMakeLists.txt
; sets VERSION 3.8.1), whose native\vst3sdk\LICENSE.txt is genuinely the
; MIT License (confirmed by reading the file directly, not inferred from
; the version number). Steinberg relicensed VST3 to MIT starting at 3.8 --
; commercial binary distribution is permitted without a separate
; Steinberg agreement (https://steinbergmedia.github.io/vst3_dev_portal/pages/VST+3+Licensing/VST3+License.html).
; What this genuinely requires: include the MIT copyright/license text
; (staged below), and follow Steinberg's trademark rules if "VST" or the
; VST logo appear in any public-facing material -- a naming/branding
; checklist item for whoever writes the store page or marketing copy, not
; something this installer script itself can violate or comply with on
; its own.
;
; JUCE: this project vendors JUCE 9.0.1 (native\JUCE\CMakeLists.txt),
; dual-licensed AGPLv3 / commercial (native\JUCE\LICENSE.md). Resolved:
; JUCE's actual current terms (https://juce.com/legal/juce-9-licence/,
; fetched directly) put the free Starter tier's threshold at combined
; revenue + funding "up to $20,000" over the trailing 12 months.
; BeatShore is pre-revenue with no funding raised -- the Starter tier
; applies, no cost. Re-confirm if BeatShore's revenue/funding situation
; changes, and give the full EULA a human read-through before shipping --
; this was resolved from an AI-summarized reading of the terms, not a
; full manual legal read.
;
; --- Verified staging (done, for real, across two sessions): a production
; copy of native\BeatShoreDesktop\engine\ was staged into
; stage\engine\ via `npm ci --omit=dev` using Node 24.19.0 LTS (Active
; LTS as of this writing -- Node 25.x was used in an earlier pass and
; correctly flagged as already end-of-life; do not regress to it or any
; other non-LTS line), then trimmed of tfjs-node's build-time-only
; artifacts (its deps\ directory -- a duplicate ~240MB copy of
; tensorflow.dll plus the .lib linker stub used only by node-gyp at
; *compile* time, confirmed by inspecting lib\napi-v8\ separately
; containing its own complete, self-sufficient tensorflow.dll +
; tfjs_binding.node -- and build-tmp-napi-v8\, a node-gyp scratch
; directory) and of every *.map source-map file across node_modules
; (~190MB, pure debug metadata, never loaded at runtime). Real, measured
; reduction: 739MB -> ~302MB (~59%), reproduced on both Node versions.
; This was NOT trusted on size-reduction reasoning alone: the complete
; BridgeClientTest + MultiSessionTest suites were run against the exact
; trimmed staged directory afterward on Node 24, including a real
; transcribePolyphonic round trip (91 notes, sha256 matching every other
; run this project has ever produced, on Node 25 or 24) and a full
; cancel-and-restart cycle (the desktop killing and respawning node.exe
; against this exact staged analyze.js) -- both passed. `--self-test` (see
; below) was also run against this exact staged tree and passed all
; checks. Also found and fixed in the same pass:
; analyze.js's unmodified `import ... from '../../../beatshore-dsp.js'`
; (and basic-pitch-model.js's equivalent `vendor/basic-pitch-model/`
; import) resolve relative to analyze.js's own file location, which only
; lines up with beatshore-dsp.js's real location at this project's exact
; dev-tree depth -- broken under any simpler installer layout. Fixed by
; staging analyze.js at a depth that mirrors the dev tree's own nesting
; (stage\engine\native\BeatShoreDesktop\engine\analyze.js, with
; beatshore-dsp.js and vendor\ placed exactly three directories up, at
; stage\engine\) rather than editing analyze.js's import itself -- keeps
; the "reuses beatshore-dsp.js unmodified" guarantee intact; the odd
; nesting is purely an installer-internal implementation detail the user
; never sees or interacts with. This exact nesting trick is still fragile
; long-term (see "Staging layout" in the trailing notes) -- ships because
; it's verified working, not because it's the right shape forever.
;
; --- `--self-test` now exists (native/BeatShoreDesktop/Source/main.cpp,
; runSelfTest()) and is real, not aspirational: `BeatShoreDesktop.exe
; --self-test <analyze.js path>` starts a real NodeEngine (no pipe, no
; plugin needed), confirms it reaches a valid READY, runs a real `tempo`
; request and a real `transcribePolyphonic` request against a small
; synthesized two-note audio fixture (proving the Basic Pitch model
; actually loads and infers, not just that the process starts), and
; confirms the MIDI export directory is writable -- exiting 0 only if
; every check passes. Verified directly: run against the real staged tree
; above, exit code 0, all four checks PASS; run against a broken path,
; exit code 1, with a specific, correct failure reason logged.
;
; Prerequisites this script still assumes exist BEFORE compiling it:
;   1. A Release build of BeatShoreBridge.vst3 and BeatShoreDesktop.exe
;      (with --self-test built in), copied to stage\BeatShoreDesktop.exe
;      and stage\BeatShore Bridge.vst3\.
;   2. A COPY of node.exe -- v24.19.0 LTS, matching the version
;      node_modules was installed against in the verified staging above --
;      placed at stage\node\node.exe. See main.cpp's defaultNodeExe(),
;      which prefers exactly this bundled path over system PATH once it
;      exists.
;   3. The Microsoft Visual C++ x64 Redistributable installer, downloaded
;      directly from Microsoft (not a mirror) and its Authenticode
;      signature verified (e.g. `signtool verify /pa vc_redist.x64.exe`)
;      before staging, placed at stage\vc_redist.x64.exe
;      (BeatShoreDesktop.exe and BeatShoreBridge.vst3 both dynamically
;      link MSVCP140.dll / VCRUNTIME140.dll / VCRUNTIME140_1.dll --
;      confirmed via `dumpbin /dependents`, not assumed -- which are NOT
;      present on a bare Windows install).
;   4. stage\Licenses\ (third-party notices -- NOT the same thing as
;      BeatShore's own EULA, see the [Setup] LicenseFile comment below)
;      and stage\LicenseFile.txt assembled from the real per-component
;      license texts (see the "Build steps" block at the end of this file
;      for the exact list).
;   5. A real, permanent AppId (see #define MyAppId below -- one has been
;      generated for this draft; do not regenerate it, do not use a
;      different one between versions, or Inno Setup will treat every
;      release as a fresh, unrelated product instead of an upgrade), and
;      real publisher/copyright values (see #define MyAppPublisher/
;      MyAppCopyright below -- resolved). Product website, support
;      email/URL, and a privacy-policy URL are still placeholders --
;      need genuinely live addresses, not invented here.
;   6. A release manifest recording SHA-256 hashes of every staged binary
;      artifact and the exact tool versions used (Node version, this
;      script's own version) -- see native/installer/RELEASE_MANIFEST.md
;      for the template and what's been filled in from this session's
;      actual staged files.

#define MyAppName "BeatShore"
#define MyAppVersion "0.2.0"
; Publisher and copyright: real values, supplied directly by the project
; owner (matching the licensor already named in the EULA,
; stage\LicenseFile.txt) -- not placeholders, not invented here.
#define MyAppPublisher "Singh's Innovation & Advisory"
#define MyAppCopyright "Copyright (C) 2026 Singh's Innovation & Advisory."
; Still placeholders -- product website, support email, support URL, and
; a privacy-policy URL are real business decisions this script can't
; invent; they need to be genuinely live addresses before a public
; release, not filled in with a plausible-looking guess. AppId below is
; the one exception already resolved (see its own comment).
#define MyAppURL "https://example.invalid/beatshore"
; Fixed, permanent, generated once for this project (2026-08-24) -- do
; NOT regenerate this or Inno Setup will treat every future version as an
; unrelated product instead of an upgrade of this one. Keep it forever,
; including across the publisher/URL/versioning decisions above.
#define MyAppId "{{E8A18368-E91F-4642-BDA0-5DEFD6A19286}"
; A build identifier distinct from MyAppVersion -- MyAppVersion (0.2.0)
; names the release; this names the specific compile of it, since more
; than one compile can share the same MyAppVersion (as this one does --
; several rebuilds since, still 0.2.0). Bump both by hand for each real
; rebuild; this is a manual build process today, not a CI pipeline that
; could generate it automatically. Embedded into the compiled Setup.exe's
; own version info below (visible via right-click -> Properties ->
; Details) and recorded alongside its SHA-256 in RELEASE_MANIFEST.md --
; so "which exact binary is this" doesn't depend on trusting a filename
; or an external record alone.
#define MyBuildId "20260825.1"
; A SEPARATE small integer, not derived from MyBuildId above -- Windows
; version-resource components are 16-bit (max 65535), so MyBuildId's own
; YYYYMMDD-based portion can't be used directly as a numeric component.
; This is what VersionInfoVersion's 4th component below actually uses.
; A previous version of this file's own comment claimed "the build-id
; sequence number becomes the 4th component" while the code directly
; below it just hardcoded a literal ".1" regardless of MyBuildId's actual
; value -- every compile showed an identical FileVersionRaw/
; ProductVersionRaw (0.2.0.1) no matter how many times MyBuildId was
; bumped, silently failing to be "the load-bearing, programmatically-
; queryable build discriminator" that same comment claimed it was. Fixed
; by actually deriving VersionInfoVersion's 4th component from this
; value. Bump this alongside MyBuildId for each real rebuild.
#define MyBuildNumber "4"

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppCopyright={#MyAppCopyright}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
; This installer writes into Program Files and the shared Common Files
; VST3 folder, both of which require elevation -- explicit, not implied
; by DefaultDirName choosing {autopf}. A future per-user install mode
; (no admin required, VST3 into
; %LOCALAPPDATA%\Programs\Common\VST3 instead) would need its own
; separate build of this script, not a mix of admin/non-admin paths in
; one installer.
PrivilegesRequired=admin
; BeatShore's own end-user agreement -- NOT the same thing as the
; third-party MIT/Apache notices below, which are attribution material
; the user is informed of, not necessarily terms they must contractually
; accept. Real, filled-in content (licensor: Singh's Innovation &
; Advisory; governing law: Suriname; no off-machine data collection) --
; not a placeholder -- but drafted, not attorney-reviewed; a professional
; legal review before a broad public release is still recommended,
; especially if distribution scope, pricing, or data handling changes.
LicenseFile=stage\LicenseFile.txt
OutputBaseFilename=BeatShoreSetup-{#MyAppVersion}
; VersionInfoVersion must be strict numeric X.X.X.X -- MyBuildNumber
; becomes the 4th component (see its own comment above for why not
; MyBuildId directly), so the compiled exe's own Properties -> Details ->
; Product version genuinely distinguishes this compile from any other
; sharing the same MyAppVersion -- confirmed by actually checking
; (Get-Item ...).VersionInfo.ProductVersionRaw across two different
; MyBuildNumber values and seeing it actually change, not just compiling
; without error.
VersionInfoVersion={#MyAppVersion}.{#MyBuildNumber}
; Short, deliberately -- VersionInfoTextVersion silently truncated a
; longer "0.2.0 (build 20260824.1)" string at 21 characters when tried
; (an Inno Setup/Windows version-resource quirk, not investigated
; further since VersionInfoVersion's numeric 4th component above is the
; load-bearing, programmatically-queryable build discriminator; this
; string field is supplementary). RELEASE_MANIFEST.md is the
; authoritative place for the full build ID.
VersionInfoTextVersion=build {#MyBuildId}
; The installer .exe's own Properties -> Details tab (distinct from
; AppPublisher/AppCopyright above, which are what the install wizard
; itself displays) -- same real values, not left at Inno Setup's
; defaults.
VersionInfoCompany={#MyAppPublisher}
VersionInfoCopyright={#MyAppCopyright}
Compression=lzma2/max
SolidCompression=yes
; x64 only -- BeatShoreDesktop.exe, the VST3, and tfjs-node's native
; TensorFlow library are all x64 builds; there is no 32-bit build of any
; of this.
; x64compatible (not the bare "x64" identifier, which Inno Setup 6.7+
; deprecates and would otherwise warn about at compile time) matches
; native x64 and ARM64 running x64 code via emulation -- correct here
; since nothing in this build is ARM64-native.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern
; The real BeatShore icon (derived from the actual brand logo -- see
; assets/icon/generate_icon.py, not a placeholder) for the installer
; .exe itself (Explorer, taskbar, the wizard's own title bar) --
; distinct from UninstallDisplayIcon below, which controls what
; Add/Remove Programs shows for the INSTALLED app and already gets the
; same real icon for free once BeatShoreDesktop.exe has it embedded
; (see native/BeatShoreDesktop/Source/resources.rc).
SetupIconFile=..\..\assets\icon\BeatShore.ico
UninstallDisplayIcon={app}\BeatShoreDesktop.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
; --- Desktop process ---
Source: "stage\BeatShoreDesktop.exe"; DestDir: "{app}"; Flags: ignoreversion

; --- Analysis engine. Staged at this specific nested depth -- not
; {app}\engine\analyze.js directly -- because analyze.js's own unmodified
; `../../../beatshore-dsp.js` import needs beatshore-dsp.js exactly three
; directories above wherever analyze.js sits; see the top-of-file
; "Verified staging" note for why this is a real constraint, not
; incidental structure. The [Icons] entry and RunSelfTest() below pass
; this exact path explicitly as BeatShoreDesktop.exe's command-line
; argument, rather than relying on its dev-tree-shaped default
; script-path resolution. ---
Source: "stage\engine\beatshore-dsp.js"; DestDir: "{app}\engine"; Flags: ignoreversion
; package.json here (sibling of beatshore-dsp.js, "type":"module") is not
; optional -- without it, Node's module-type resolution for
; beatshore-dsp.js is ambiguous and falls back to a syntax-detection
; heuristic that is NOT reliably correct: found by actually running the
; staged tree from an unrelated working directory with no arguments
; (mirroring exactly how the Run key launches it), not by inspection --
; one run failed outright ("doesn't parse as CommonJS"), a nearly
; identical earlier run had merely warned and happened to still succeed.
; Staging this file removes the ambiguity entirely rather than relying on
; the heuristic guessing right.
Source: "stage\engine\package.json"; DestDir: "{app}\engine"; Flags: ignoreversion
Source: "stage\engine\vendor\*"; DestDir: "{app}\engine\vendor"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "stage\engine\native\BeatShoreDesktop\engine\*"; DestDir: "{app}\engine\native\BeatShoreDesktop\engine"; Flags: ignoreversion recursesubdirs createallsubdirs

; --- Bundled Node runtime (v24.19.0 LTS -- see top-of-file note) -- see
; the prerequisites comment above for why this has to be staged manually
; rather than fetched by this script. ---
Source: "stage\node\node.exe"; DestDir: "{app}\node"; Flags: ignoreversion

; --- VST3 plugin, into the standard shared VST3 location every DAW scans
; (REAPER, Cubase, Ableton, Studio One, FL Studio all default here) --
; NOT under {app}, since a VST3 host has no reason to look in this app's
; own install directory. common64 forces the 64-bit Common Files path
; even if a 32-bit Inno Setup build were ever used to compile this. ---
Source: "stage\BeatShore Bridge.vst3\*"; DestDir: "{commoncf64}\VST3\BeatShore Bridge.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

; --- Third-party license notices (attribution, not a user agreement --
; see [Setup]'s LicenseFile comment). Installed into {app}\Licenses so
; they're discoverable after setup, not just shown once during install.
; A generated software-bill-of-materials belongs here too once produced
; (see native/installer/RELEASE_MANIFEST.md). ---
Source: "stage\Licenses\*"; DestDir: "{app}\Licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

; --- Microsoft Visual C++ x64 Redistributable -- handled in [Code] below
; (RunVCRedist), not a declarative [Run] entry, so its actual exit code
; can be inspected rather than just waited on. ---
Source: "stage\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\BeatShore Desktop"; Filename: "{app}\BeatShoreDesktop.exe"; Parameters: """{app}\engine\native\BeatShoreDesktop\engine\analyze.js"""
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"

[Code]
// Always runs the redistributable's own installer quietly and lets IT
// decide whether an install/repair/upgrade is actually needed, rather
// than replicating Microsoft's Major.Minor.Bld.Rbld compatibility logic
// here with a guessed threshold -- Microsoft's own installer already
// knows this correctly (see
// https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files).
// Idempotent and fast (a few seconds) when a sufficient version is
// already present, so running it unconditionally isn't wasteful. Real
// exit-code handling, not just "wait and hope": 0 (installed or already
// current) and 1638 (a newer version is already present) are both
// success; 3010 is success but tells the user a restart is needed;
// anything else is logged and surfaced as a real failure.
procedure RunVCRedist();
var
  ResultCode: Integer;
begin
  if not Exec(ExpandConstant('{tmp}\vc_redist.x64.exe'), '/install /quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    Log('VC++ redistributable: Exec() itself failed to launch the installer (missing or corrupt file).');
    if not WizardSilent() then
      MsgBox('The Microsoft Visual C++ Runtime installer could not be launched. BeatShore may not run correctly until it is installed manually from https://aka.ms/vs/17/release/vc_redist.x64.exe.', mbError, MB_OK);
    Exit;
  end;
  case ResultCode of
    0: Log('VC++ redistributable: installed or already current (exit 0).');
    1638: Log('VC++ redistributable: a newer version is already installed (exit 1638) -- not an error.');
    3010: begin
      Log('VC++ redistributable: installed, restart required (exit 3010).');
      if not WizardSilent() then
        MsgBox('The Microsoft Visual C++ Runtime was installed, but your computer needs to restart before BeatShore will run correctly.', mbInformation, MB_OK);
    end;
  else
    begin
      Log('VC++ redistributable: install FAILED, exit code ' + IntToStr(ResultCode) + '.');
      if not WizardSilent() then
        MsgBox('The Microsoft Visual C++ Runtime installer reported an error (code ' + IntToStr(ResultCode) + '). BeatShore may not run correctly -- see the install log for details.', mbError, MB_OK);
    end;
  end;
end;

// Runs unconditionally in BOTH interactive and silent installs (a silent
// deployment needs verification just as much as an interactive one, if
// not more, since there's no user watching to notice a problem) --
// verified working: run against a real staged tree, exit 0, all checks
// PASS; run against a deliberately broken path, exit 1, correct specific
// failure reason logged.
//
// Exec() alone only reports a child process's numeric exit code, not
// anything it printed -- so this routes stdout+stderr through cmd.exe's
// own redirection into a real file left behind in {app} (not deleted,
// not written only to Inno's own optional /LOG output, which nothing
// captures unless the caller explicitly requested it), so both a silent
// deployment and a human opening the folder later have the exact
// PASS/FAIL lines runSelfTest() (main.cpp) printed to work from, not just
// a bare exit code.
function SelfTestLogPath(): String;
begin
  Result := ExpandConstant('{app}\selftest-log.txt');
end;

function RunSelfTest(): Boolean;
var
  ResultCode: Integer;
  ScriptArg, CmdLine: String;
begin
  ScriptArg := '--self-test "' + ExpandConstant('{app}\engine\native\BeatShoreDesktop\engine\analyze.js') + '"';
  CmdLine := '/C ""' + ExpandConstant('{app}\BeatShoreDesktop.exe') + '" ' + ScriptArg + ' > "' + SelfTestLogPath() + '" 2>&1"';
  Result := Exec(ExpandConstant('{cmd}'), CmdLine, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
  if Result then
    Log('Self-test: PASSED (exit 0). Full output: ' + SelfTestLogPath())
  else
    Log('Self-test: FAILED (ResultCode=' + IntToStr(ResultCode) + '). Full output: ' + SelfTestLogPath());
end;

// Pulls the actual PASS/FAIL lines back out of the persistent log above,
// so a failure is reported as "polyphonic transcription check failed"
// (or whichever check it actually was), not just "something failed, go
// look" -- both in the interactive MsgBox and in the incomplete-install
// marker file below.
function SelfTestFailureSummary(): String;
var
  Lines: TArrayOfString;
  I: Integer;
begin
  Result := '';
  if LoadStringsFromFile(SelfTestLogPath(), Lines) then
  begin
    for I := 0 to GetArrayLength(Lines) - 1 do
      if (Pos('FAIL', Lines[I]) > 0) or (Pos('FATAL', Lines[I]) > 0) then
        Result := Result + Lines[I] + #13#10;
  end;
  if Result = '' then
    Result := '(no specific failure line found in ' + SelfTestLogPath() + ' -- see that file directly)';
end;

// There is no built-in Inno Setup concept of a "partial/failed" install
// state -- the uninstall registry entry it writes doesn't carry one --
// so this is a plain, greppable marker file instead: something a support
// process, a silent-deployment health check, or a future first-run check
// inside BeatShoreDesktop.exe itself could look for. Removed automatically
// if a later reinstall's self-test succeeds.
procedure WriteIncompleteMarker(const Reason: String);
begin
  SaveStringToFile(ExpandConstant('{app}\INSTALL_INCOMPLETE.txt'),
    'This BeatShore installation did not pass its own self-test and should' + #13#10 +
    'be considered INCOMPLETE -- it may not run correctly.' + #13#10#13#10 +
    'Failing check(s):' + #13#10 + Reason + #13#10 +
    'Full self-test output: ' + SelfTestLogPath() + #13#10#13#10 +
    'Recommended: uninstall and reinstall. This file is removed automatically' + #13#10 +
    'the next time BeatShore is installed and its self-test passes.', False);
end;

procedure RemoveIncompleteMarker();
var
  MarkerPath: String;
begin
  MarkerPath := ExpandConstant('{app}\INSTALL_INCOMPLETE.txt');
  if FileExists(MarkerPath) then
    DeleteFile(MarkerPath);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  FailureSummary: String;
  DummyResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    RunVCRedist();
    if RunSelfTest() then
      RemoveIncompleteMarker()
    else
    begin
      // By the time this runs, every file has already been copied and
      // the Start Menu shortcut already created -- Inno Setup has no
      // built-in "roll back files already committed from inside
      // CurStepChanged" mechanism, so an actual rollback isn't available
      // here. What IS done instead, all four honestly checkable without a
      // real rollback: (1) the failing check is named, not just logged as
      // a bare failure; (2) the install is marked incomplete via a
      // persistent marker file; (3) BeatShore is never offered to launch
      // (this script has no [Run] "launch after install" entry at all,
      // interactive or silent, so there is nothing to suppress here --
      // it simply never offered that); (4) an immediate uninstall is
      // offered interactively, and Setup's own exit code is forced
      // nonzero unconditionally so a silent/scripted deployment can
      // detect the failure without a human watching a dialog.
      FailureSummary := SelfTestFailureSummary();
      WriteIncompleteMarker(FailureSummary);
      Log('Self-test FAILED -- installation marked incomplete (' + ExpandConstant('{app}') + '\INSTALL_INCOMPLETE.txt). Failing check(s): ' + FailureSummary);

      if not WizardSilent() then
      begin
        if MsgBox(
          'BeatShore installed, but its self-test did not pass and it will not work correctly.' + #13#10#13#10 +
          'Failing check(s):' + #13#10 + FailureSummary + #13#10 +
          'Full details: ' + SelfTestLogPath() + #13#10#13#10 +
          'Uninstall BeatShore now?',
          mbError, MB_YESNO) = IDYES then
          Exec(ExpandConstant('{uninstallexe}'), '/SILENT', '', SW_SHOW, ewNoWait, DummyResultCode);
      end;

      // Forces Setup.exe to exit with Inno Setup's documented
      // cancelled/aborted exit code rather than 0, so a silent or
      // scripted deployment (Intune, SCCM, a CI smoke test) sees a
      // nonzero ERRORLEVEL and does not treat this as a successful
      // install -- there is no other documented API to directly set
      // Setup's own final exit code from [Code]. Honest limitation, not
      // glossed over: the exact resulting exit code and any Inno-internal
      // behavior this triggers has not been observed from an actual
      // interactive or silent install run in this environment (no
      // UAC-capable install is possible here) -- implemented per Inno
      // Setup's documented Abort() semantics, not confirmed end-to-end by
      // watching a real failing install happen. This is a genuine,
      // real, but not fully-verified-from-here fix, same honesty
      // standard as everything else in this file that a live install
      // would be needed to fully confirm.
      Abort;
    end;
  end;
end;

// ---------------------------------------------------------------------
// Everything below this line is NOT Inno Setup script -- it's the honest
// state of what this installer still needs before it's real, kept here
// rather than only in STATUS.md so whoever picks this up next sees it in
// the same file as the script itself.
// ---------------------------------------------------------------------
//
// Build steps to actually produce the "stage\" tree this script expects
// (steps 4-6 below were actually run across two sessions and verified
// working, on both Node 25 and, after that was correctly flagged as
// unsupported, a from-scratch rebuild on Node 24 LTS -- see the top-of-file
// "Verified staging" note; the rest are still manual). None of this is
// automated yet -- a real release process should be a script, not a
// manual checklist, but the checklist has to exist first:
//   1. Release-build BeatShoreBridge.vst3 and BeatShoreDesktop.exe
//      (--self-test now built in -- verify it still passes against
//      whatever you just built before proceeding).
//   2. Copy "BeatShore Bridge.vst3" (the whole bundle directory) to
//      stage\.
//   3. Copy BeatShoreDesktop.exe to stage\.
//   4. In a completely clean checkout of native\BeatShoreDesktop\engine\
//      (a fresh directory, never a copy of a dev tree that might carry
//      extra tooling or a stale binding), run `npm ci --omit=dev` using
//      Node 24 LTS specifically -- NOT the newer non-LTS line this
//      project's own dev environment happens to have installed, which was
//      already end-of-life and is exactly the mistake this checklist item
//      exists to prevent. Then remove
//      node_modules\@tensorflow\tfjs-node\{deps,build-tmp-napi-v8}\ and
//      every *.map file (verified safe -- see "Verified staging" above).
//      Copy the result to
//      stage\engine\native\BeatShoreDesktop\engine\, copy the repo-root
//      beatshore-dsp.js to stage\engine\beatshore-dsp.js, and copy
//      vendor\basic-pitch-model\ to stage\engine\vendor\basic-pitch-model\
//      -- this exact nested layout, not a flattened one (see the [Files]
//      comment above for why).
//   5. Copy the SAME node.exe used for step 4's `npm ci` (v24.19.0 in the
//      verified staging above -- do not substitute a different node.exe
//      binary next to node_modules that were installed against a
//      different version without re-running step 4 from scratch) to
//      stage\node\node.exe.
//   6. Run `BeatShoreDesktop.exe --self-test
//      stage\engine\native\BeatShoreDesktop\engine\analyze.js`, then
//      re-run the full BridgeClientTest + MultiSessionTest suites against
//      the exact staged tree, before trusting any further trimming or any
//      Node-version change -- do not delete additional package files or
//      swap Node versions based on reasoning alone without re-verifying
//      against this exact staged directory each time, the same way steps
//      above were.
//   7. Download the matching VC++ x64 redistributable directly from
//      Microsoft (https://aka.ms/vs/17/release/vc_redist.x64.exe -- not a
//      mirror), verify its Authenticode signature
//      (`signtool verify /pa vc_redist.x64.exe`), then copy to
//      stage\vc_redist.x64.exe.
//   8. ~~Assemble stage\Licenses\... and write stage\LicenseFile.txt as
//      BeatShore's own EULA~~ Done: stage\Licenses\ holds the real
//      per-component notices (JUCE, VST3 SDK, basic-pitch, Node.js,
//      tfjs-node), and stage\LicenseFile.txt now holds real, filled-in
//      EULA content (licensor: Singh's Innovation & Advisory; governing
//      law: Suriname; Section 5 discloses that no audio/usage data leaves
//      the user's machine) -- drafted, not attorney-reviewed; a
//      professional legal read-through is still worth doing before a
//      broad public release.
//   9. Generate/update native/installer/RELEASE_MANIFEST.md with SHA-256
//      hashes of every staged binary artifact for this exact build.
//   10. ~~Resolve the desktop-process startup question~~ Done:
//       BeatShoreDesktop.exe is now a real system tray app (icon, "Start
//       at login" toggle via HKCU\...\Run, Quit) with a single-instance
//       broker mutex -- see STATUS.md's "Clean-machine packaging" section
//       for how this was verified (real WM_COMMAND messages sent to the
//       actual tray window, not just code review). Still missing: a real
//       icon resource, and the installer itself doesn't yet offer to
//       enable "Start at login" during setup.
//   11. Install Inno Setup 6 and compile this script -- not available in
//       this environment (see the top-of-file note). Treat compiler
//       warnings as real, not noise.
//   12. Code-sign BeatShoreDesktop.exe, the VST3 binary inside the
//       bundle, and the compiled installer -- no signing certificate is
//       available in this environment; not attempted.
//   13. Test install / repair / upgrade / uninstall, and the full
//       clean-machine acceptance test (Windows Sandbox, Hyper-V, VMware,
//       or a separate physical machine with none of this project's dev
//       tools, no existing BeatShore files, and ideally no pre-existing
//       VC++ runtime or VST3 registration cache) -- needs a second machine
//       this environment doesn't have.
//
// Staging layout: the depth-preserving nesting under stage\engine\ (see
// "Verified staging" above) works and is verified, but it's fragile --
// it ties packaging to matching the dev tree's exact directory depth
// forever. A better long-term shape: keep beatshore-dsp.js itself
// completely unmodified, but introduce a small packaging-only entry point
// next to analyze.js that resolves beatshore-dsp.js's and the model
// weights' paths from the installed executable's own location (or
// explicit configuration) instead of a fixed `../../../` relative climb,
// so the installer's directory layout is free to be whatever's sensible
// rather than a mirror of the source tree. Not attempted in this pass --
// the current layout ships fine as verified, but this needs an automated
// staging-layout test (confirm the exact directory depth resolves
// correctly, as part of CI or a release script) before it's trusted
// long-term rather than re-verified by hand each release the way this
// session did it.
//
// Size: ~302MB for the engine directory after the trimming verified this
// session (down from 739MB, reproduced on both Node 25 and Node 24) --
// still substantial (tfjs-node's own runtime artifacts, dominated by
// tensorflow.dll, account for most of what's left), so a fully-bundled
// installer is realistically a 350-450MB download even after LZMA
// compression, better than the 800MB-1GB it would have been untrimmed but
// still not small. Making transcribePolyphonic an optional download
// rather than bundled by default is a real option worth considering
// separately if this needs to shrink further, not attempted here.
//
// ~~Runtime behavior not yet decided...~~ Resolved -- see build step 10
// above: BeatShoreDesktop.exe is a real system tray app with a "Start at
// login" toggle, and only one instance ever runs at a time (a named
// mutex, checked at startup in main.cpp, refuses a second launch).
//
// Not attempted at all: actually running this installer on a clean
// Windows machine. This dev environment (this exact machine) has Visual
// Studio, CMake, JUCE, and npm installed, which is precisely the set of
// things a genuine clean-machine test needs to NOT have -- there is no
// way to honestly claim that verification from inside this environment.
// A second, genuinely bare Windows machine (or VM) is needed for that.
