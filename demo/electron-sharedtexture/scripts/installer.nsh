!macro customInstall
  IfFileExists "$PROGRAMFILES64\Bonjour SDK\Include\dns_sd.h" bonjourSdkDone 0
  IfFileExists "$PROGRAMFILES\Bonjour SDK\Include\dns_sd.h" bonjourSdkDone 0
  IfFileExists "$INSTDIR\resources\third-party\bonjour\bonjoursdksetup.exe" 0 bonjourSdkMissing

  StrCpy $1 "$TEMP\UxPlay-BonjourSDK-install.log"
  DetailPrint "Installing Bonjour SDK silently..."
  DetailPrint "Bonjour SDK install log: $1"
  ExecWait '"$INSTDIR\resources\third-party\bonjour\bonjoursdksetup.exe" /qn /norestart /l*v "$1"' $0
  StrCmp $0 0 bonjourSdkVerify 0
  StrCmp $0 3010 bonjourSdkVerify 0
  MessageBox MB_ICONEXCLAMATION|MB_OK "Bonjour SDK installer exited with code $0. UxPlay SharedTexture was installed, but Bonjour SDK may need to be installed manually. Installer log: $1"
  Goto bonjourSdkDone

bonjourSdkVerify:
  IfFileExists "$PROGRAMFILES64\Bonjour SDK\Include\dns_sd.h" bonjourSdkDone 0
  IfFileExists "$PROGRAMFILES\Bonjour SDK\Include\dns_sd.h" bonjourSdkDone 0
  MessageBox MB_ICONEXCLAMATION|MB_OK "Bonjour SDK silent installer finished, but the SDK files were not detected. UxPlay SharedTexture was installed, but Bonjour SDK may need to be installed manually. Installer log: $1"
  Goto bonjourSdkDone

bonjourSdkMissing:
  MessageBox MB_ICONEXCLAMATION|MB_OK "Bonjour SDK installer was not found in the application resources. UxPlay SharedTexture was installed, but Bonjour SDK may need to be installed manually."

bonjourSdkDone:
!macroend
