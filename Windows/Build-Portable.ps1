$ErrorActionPreference = 'Stop'

$target = $null
if ($env:PAKR_APP_URL.Length -gt 2048 -or
    -not [Uri]::TryCreate($env:PAKR_APP_URL, [UriKind]::Absolute, [ref]$target) -or
    $target.Scheme -ne 'https' -or $target.UserInfo) {
    throw 'Invalid HTTPS target URL'
}
if ($env:PAKR_APP_NAME -notmatch '\A[\p{L}\p{M}\p{N} _.-]{1,60}\z' -or
    [string]::IsNullOrWhiteSpace($env:PAKR_APP_NAME)) {
    throw 'Invalid app name'
}
if ($env:PAKR_APP_ID -cnotmatch '\A[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)+\z' -or $env:PAKR_APP_ID.Length -gt 100) {
    throw 'Invalid app identifier'
}
if ($env:PAKR_VERSION -notmatch '\A[0-9]{1,5}(\.[0-9]{1,5}){2}\z' -or
    ($env:PAKR_VERSION.Split('.') | Where-Object { [int]$_ -gt 65535 })) {
    throw 'Invalid version'
}

$iconPath = Join-Path $env:RUNNER_TEMP 'Pakr.App.ico'
& python Windows/Process-Icon.py $iconPath
if ($LASTEXITCODE -ne 0) { throw 'Icon processing failed' }

$configPath = Join-Path $env:RUNNER_TEMP 'Pakr.AppConfig.bin'
$appName = $env:PAKR_APP_NAME.Trim()
$config = @($target.AbsoluteUri, $appName, $env:PAKR_APP_ID) -join "`n"
[IO.File]::WriteAllBytes($configPath, [Text.Encoding]::Unicode.GetBytes($config))

$version = ($env:PAKR_VERSION.Split('.') | ForEach-Object { [int]$_ }) -join ','
$resourcePath = Join-Path $env:RUNNER_TEMP 'Pakr.App.rc'
$iconResource = $iconPath.Replace('\', '/')
$configResource = $configPath.Replace('\', '/')
@"
#pragma code_page(65001)
1 ICON "$iconResource"
101 RCDATA "$configResource"
1 VERSIONINFO
FILEVERSION $version,0
PRODUCTVERSION $version,0
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904B0"
    BEGIN
      VALUE "FileDescription", "$appName\0"
      VALUE "ProductName", "$appName\0"
      VALUE "FileVersion", "$($env:PAKR_VERSION)\0"
      VALUE "ProductVersion", "$($env:PAKR_VERSION)\0"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x409, 1200
  END
END
"@ | Set-Content -LiteralPath $resourcePath -Encoding utf8BOM

$sdkVersion = '1.0.3537.50'
$sdk = Join-Path $env:RUNNER_TEMP "Microsoft.Web.WebView2.$sdkVersion"
$sdkArchive = "$sdk.zip"
Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$sdkVersion/microsoft.web.webview2.$sdkVersion.nupkg" -OutFile $sdkArchive -TimeoutSec 60
Expand-Archive -LiteralPath $sdkArchive -DestinationPath $sdk -Force

$buildDirectory = Join-Path $env:RUNNER_TEMP 'pakr-windows-build'
# CMake re-parses source paths, where Windows backslashes can become escapes.
$sdkForCmake = $sdk.Replace('\', '/')
$resourcesForCmake = $resourcePath.Replace('\', '/')
& cmake -S Windows -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 "-DPAKR_WEBVIEW2_SDK:PATH=$sdkForCmake" "-DPAKR_RESOURCES:FILEPATH=$resourcesForCmake"
if ($LASTEXITCODE -ne 0) { throw "Windows app configuration failed ($LASTEXITCODE)" }
& cmake --build $buildDirectory --config Release
if ($LASTEXITCODE -ne 0) { throw "Windows app compilation failed ($LASTEXITCODE)" }

# The artifact prefix distinguishes portable builds from historical installers.
$fileName = "Pakr-Portable-$appName.exe"
New-Item -ItemType Directory -Path 'windows-artifact' -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDirectory 'Release/Pakr.Windows.exe') -Destination (Join-Path 'windows-artifact' $fileName)
