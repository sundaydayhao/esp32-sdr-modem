param([switch]$Test)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$android=Join-Path $root 'mobile/android'
$sdk=Join-Path $env:LOCALAPPDATA 'Android/Sdk'
$env:JAVA_HOME='C:/Program Files/Android/Android Studio/jbr'
$gradle=Get-ChildItem (Join-Path $env:USERPROFILE '.gradle/wrapper/dists/gradle-8.7-bin') -Recurse -Filter gradle.bat | Select-Object -First 1 -ExpandProperty FullName
if(!$gradle){throw 'Gradle 8.7 is required. Open mobile/android in Android Studio or install Gradle 8.7.'}
if(!(Test-Path (Join-Path $sdk 'platforms/android-34/android.jar'))){throw 'Install Android SDK Platform 34 first.'}
[IO.File]::WriteAllText((Join-Path $android 'local.properties'),"sdk.dir=$($sdk.Replace('\','/'))`n",[Text.UTF8Encoding]::new($false))
$tasks=@('assembleDebug')
if($Test){$tasks+=@('testDebugUnitTest')}
Push-Location $android
try { & $gradle @tasks --console=plain; if($LASTEXITCODE){throw 'Android build/check failed.'} } finally { Pop-Location }
$output=Join-Path $root 'build/android'
New-Item -ItemType Directory -Force $output | Out-Null
Copy-Item -LiteralPath (Join-Path $android 'app/build/outputs/apk/debug/app-debug.apk') -Destination (Join-Path $output 'sdr-studio-0.1.0-debug.apk')
Get-FileHash (Join-Path $output 'sdr-studio-0.1.0-debug.apk') -Algorithm SHA256
