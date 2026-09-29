# Signs a WolfViewer Windows build with Azure Artifact Signing, in four stages that the workflow
# runs as separate steps (wolfviewer.yml, "Signing:" steps):
#
#   prepare         fetch signtool + the Artifact Signing dlib, write metadata.json, find the build
#   sign-binaries   sign the viewer's own executables in the packaging directory
#   repack          re-run NSIS on the build's own .nsi, so the installer carries the signed files
#   sign-installer  sign the rebuilt Setup.exe
#
# WHY SIGNING IS NOT DONE INSIDE THE BUILD. fs_viewer_manifest.py:62-110 can sign during packaging
# when SIGNTOOL_PATH / CODESIGNING_DLIB_PATH / CODESIGNING_METADATA_PATH are set. That packaging
# runs at the END of a ~50 minute build, and a GitHub OIDC login cannot last that long: the
# assertion azure/login hands to the Azure CLI lives 5 minutes, the CLI gets no refresh token, and
# the signing token is only requested when signtool first runs (Azure/azure-cli#28708, AADSTS700024).
# So the build packages unsigned, exactly as before, and these steps sign afterwards with a fresh
# login right before each signing stage.
#
# Every stage fails the job on any problem. An installer that silently ships unsigned is the thing
# this exists to prevent.

param(
    [Parameter(Mandatory)]
    [ValidateSet('prepare', 'sign-binaries', 'repack', 'sign-installer')]
    [string]$Stage
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Pinned tool packages, with the sha256 of the exact .nupkg that was inspected.
# Source: https://learn.microsoft.com/azure/artifact-signing/how-to-signing-integrations
#   - SignTool from Microsoft.Windows.SDK.BuildTools, minimum 10.0.22621.755
#   - the dlib from Microsoft.ArtifactSigning.Client; it needs the .NET 8 runtime
#     (bin/x64/Azure.CodeSigning.Dlib.runtimeconfig.json: tfm net8.0)
$Packages = @(
    @{ Id = 'microsoft.windows.sdk.buildtools'; Version = '10.0.28000.2705'
       Sha256 = '8bfdfb6ca2633f531cf80b5fa22512ba61a394d7988f0970db83baadc67929ed' },
    @{ Id = 'microsoft.artifactsigning.client'; Version = '1.0.128'
       Sha256 = '74bd7d27e6ce1051409c38d9b46bc8df0400ecd643d51ffbf2ac00869061e40b' }
)
$Root     = Join-Path $env:RUNNER_TEMP 'artifact-signing'
# Paths inside the two packages, as listed from the .nupkg contents.
$SignTool = Join-Path $Root 'microsoft.windows.sdk.buildtools\bin\10.0.28000.0\x64\signtool.exe'
$Dlib     = Join-Path $Root 'microsoft.artifactsigning.client\bin\x64\Azure.CodeSigning.Dlib.dll'
$Metadata = Join-Path $Root 'metadata.json'

# Written by viewer_manifest.py:1171 nsis_package_finish() into the packaging directory (--dest),
# with every installed file listed by absolute path (viewer_manifest.py:914-927 nsi_file_commands()).
$NsiName = 'firestorm_setup_tmp.nsi'

function Get-Required([string]$Name) {
    $v = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($v)) { throw "environment variable $Name is not set" }
    return $v
}

# One value from the generated .nsi. Both defines are written by viewer_manifest.py
# nsis_package_finish(): "!define INSTEXE" (:1131 version_vars) and "OutFile" (:1145 inst_vars).
function Get-NsiValue([string]$Nsi, [string]$Pattern) {
    $hits = @(Select-String -LiteralPath $Nsi -Pattern $Pattern)
    if ($hits.Count -ne 1) { throw "expected exactly one match for '$Pattern' in $Nsi, found $($hits.Count)" }
    return $hits[0].Matches[0].Groups[1].Value
}

function Get-Build {
    $nsi = Get-Required 'WV_NSI'
    if (-not (Test-Path -LiteralPath $nsi -PathType Leaf)) { throw "$nsi is gone" }
    $dir = Split-Path -Parent $nsi
    $exe = Get-NsiValue $nsi '^\s*!define INSTEXE\s+"([^"]+)"'
    # makensis resolves a relative OutFile against the script's own directory, which is also
    # where fs_viewer_manifest.py:96 fs_sign_win_installer() looks for it.
    $out = Get-NsiValue $nsi '^\s*OutFile\s+"([^"]+)"'
    return @{ Nsi = $nsi; Dir = $dir; Exe = $exe; Installer = (Join-Path $dir $out) }
}

function Invoke-Sign([string[]]$Files) {
    foreach ($f in $Files) {
        if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { throw "cannot sign $f - it does not exist" }
    }
    # Flags exactly as the Microsoft signtool example (how-to-signing-integrations, "Use SignTool
    # to sign a file"). The certificates live three days, so the timestamp is what keeps the
    # signature valid afterwards.
    & $SignTool sign /v /debug /fd SHA256 /tr 'http://timestamp.acs.microsoft.com' /td SHA256 `
        /dlib $Dlib /dmdf $Metadata @Files
    if ($LASTEXITCODE -ne 0) { throw "signtool exited with $LASTEXITCODE" }
    foreach ($f in $Files) { Assert-Signed $f }
}

# Valid signature, by the expected publisher, with a timestamp.
function Assert-Signed([string]$File) {
    $want = Get-Required 'SIGNING_SUBJECT_CN'
    $sig  = Get-AuthenticodeSignature -LiteralPath $File
    if ($sig.Status -ne 'Valid') { throw "$File signature is $($sig.Status): $($sig.StatusMessage)" }
    $cn = $sig.SignerCertificate.GetNameInfo(
        [System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
    if ($cn -ne $want) { throw "$File is signed by '$cn', expected '$want'" }
    if ($null -eq $sig.TimeStamperCertificate) { throw "$File has no timestamp" }
    Write-Host "signed OK: $File ($cn)"
}

switch ($Stage) {
    'prepare' {
        New-Item -ItemType Directory -Force -Path $Root | Out-Null
        foreach ($p in $Packages) {
            $zip = Join-Path $Root "$($p.Id).zip"
            $url = "https://api.nuget.org/v3-flatcontainer/$($p.Id)/$($p.Version)/$($p.Id).$($p.Version).nupkg"
            Invoke-WebRequest -Uri $url -OutFile $zip
            $got = (Get-FileHash -Algorithm SHA256 -LiteralPath $zip).Hash.ToLowerInvariant()
            if ($got -ne $p.Sha256) { throw "$($p.Id) $($p.Version) sha256 is $got, expected $($p.Sha256)" }
            Expand-Archive -LiteralPath $zip -DestinationPath (Join-Path $Root $p.Id) -Force
        }
        foreach ($f in $SignTool, $Dlib) {
            if (-not (Test-Path -LiteralPath $f -PathType Leaf)) { throw "$f missing after extraction" }
        }

        # Field names from the package's own metadata.sample.json. ExcludeCredentials is the list
        # from how-to-signing-integrations "Authentication", minus AzureCliCredential: azure/login
        # leaves an Azure CLI session and that is the only credential that should be tried.
        [ordered]@{
            Endpoint               = Get-Required 'ARTIFACT_SIGNING_ENDPOINT'
            CodeSigningAccountName = Get-Required 'ARTIFACT_SIGNING_ACCOUNT'
            CertificateProfileName = Get-Required 'ARTIFACT_SIGNING_PROFILE'
            CorrelationId          = "$env:GITHUB_REPOSITORY run $env:GITHUB_RUN_ID attempt $env:GITHUB_RUN_ATTEMPT"
            ExcludeCredentials     = @(
                'ManagedIdentityCredential', 'WorkloadIdentityCredential', 'SharedTokenCacheCredential',
                'VisualStudioCredential', 'VisualStudioCodeCredential', 'AzurePowerShellCredential',
                'AzureDeveloperCliCredential', 'InteractiveBrowserCredential')
        } | ConvertTo-Json | Set-Content -LiteralPath $Metadata -Encoding utf8

        $found = @(Get-ChildItem -LiteralPath $env:GITHUB_WORKSPACE -Recurse -File -Filter $NsiName)
        if ($found.Count -ne 1) {
            throw "expected one $NsiName in the workspace, found $($found.Count): $(($found | ForEach-Object FullName) -join ', ')"
        }
        "WV_NSI=$($found[0].FullName)" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
        Write-Host "packaging directory: $($found[0].DirectoryName)"
    }

    'sign-binaries' {
        $b = Get-Build
        # The same set Firestorm signs, fs_viewer_manifest.py:67-73 fs_sign_win_binaries().
        Invoke-Sign @(
            (Join-Path $b.Dir 'slplugin.exe'),
            (Join-Path $b.Dir 'SLVoice.exe'),
            (Join-Path $b.Dir 'llwebrtc.dll'),
            (Join-Path $b.Dir 'llplugin\dullahan_host.exe'),
            (Join-Path $b.Dir $b.Exe)
        )
    }

    'repack' {
        $b = Get-Build
        # Same search as viewer_manifest.py:1193-1199.
        $makensis = $null
        foreach ($pf in $env:ProgramFiles, ${env:ProgramFiles(x86)}) {
            foreach ($sub in 'NSIS', 'NSIS\Unicode') {
                $candidate = Join-Path $pf "$sub\makensis.exe"
                if (-not $makensis -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { $makensis = $candidate }
            }
        }
        if (-not $makensis) { throw 'makensis.exe not found' }
        if (-not (Test-Path -LiteralPath $b.Installer -PathType Leaf)) { throw "the build produced no $($b.Installer)" }

        $before = (Get-Item -LiteralPath $b.Installer).LastWriteTimeUtc
        # Same arguments as viewer_manifest.py:1201.
        & $makensis /V2 $b.Nsi
        if ($LASTEXITCODE -ne 0) { throw "makensis exited with $LASTEXITCODE" }
        $after = (Get-Item -LiteralPath $b.Installer).LastWriteTimeUtc
        if ($after -le $before) { throw "makensis did not rewrite $($b.Installer)" }
        Write-Host "rebuilt $($b.Installer)"
    }

    'sign-installer' {
        $b = Get-Build
        Invoke-Sign @($b.Installer)
    }
}
