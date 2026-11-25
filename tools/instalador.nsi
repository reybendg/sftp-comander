; Instalador de SFTP Commander para Windows (NSIS 3, MUI2).
; Se arma con: make windows  (o makensis -DVERSION_APP=x.y.z tools/instalador.nsi)
; Instala por usuario en %LOCALAPPDATA%\Programs: no pide permisos de administrador.
Unicode true
!include "MUI2.nsh"

!ifndef VERSION_APP
  !error "Falta -DVERSION_APP=x.y.z"
!endif
!define APP      "SFTP Commander"
!define RAIZ     ".."
!define CARPETA  "sftp-commander-${VERSION_APP}-windows-x64"
!define DESINST  "Software\Microsoft\Windows\CurrentVersion\Uninstall\sftp-commander"

Name "${APP} ${VERSION_APP}"
OutFile "${RAIZ}\dist\sftp-commander-${VERSION_APP}-instalador.exe"
InstallDir "$LOCALAPPDATA\Programs\${APP}"
InstallDirRegKey HKCU "Software\sftp-commander" "Carpeta"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "${APP} ${VERSION_APP}"

VIProductVersion "${VERSION_APP}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "FileDescription" "Instalador de ${APP}"
VIAddVersionKey "FileVersion" "${VERSION_APP}"
VIAddVersionKey "ProductVersion" "${VERSION_APP}"
VIAddVersionKey "LegalCopyright" "GPL-3.0"

!define MUI_ICON   "${RAIZ}\data\sftp-commander.ico"
!define MUI_UNICON "${RAIZ}\data\sftp-commander.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\sftp-commander.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Abrir ${APP}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${RAIZ}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "Spanish"

Section "${APP} (obligatorio)" SecApp
  SectionIn RO
  ; una versión anterior en la misma carpeta: se reemplaza entera
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\lib"
  RMDir /r "$INSTDIR\share"
  SetOutPath "$INSTDIR"
  File /r /x "SFTP Commander.cmd" "${RAIZ}\dist\${CARPETA}\*.*"

  CreateDirectory "$SMPROGRAMS\${APP}"
  CreateShortcut "$SMPROGRAMS\${APP}\${APP}.lnk" "$INSTDIR\bin\sftp-commander.exe" "" "$INSTDIR\sftp-commander.ico"
  CreateShortcut "$SMPROGRAMS\${APP}\Desinstalar ${APP}.lnk" "$INSTDIR\desinstalar.exe"

  WriteUninstaller "$INSTDIR\desinstalar.exe"
  WriteRegStr HKCU "Software\sftp-commander" "Carpeta" "$INSTDIR"
  ; aparece en Configuración > Aplicaciones
  WriteRegStr   HKCU "${DESINST}" "DisplayName" "${APP}"
  WriteRegStr   HKCU "${DESINST}" "DisplayVersion" "${VERSION_APP}"
  WriteRegStr   HKCU "${DESINST}" "DisplayIcon" "$INSTDIR\sftp-commander.ico"
  WriteRegStr   HKCU "${DESINST}" "Publisher" "${APP}"
  WriteRegStr   HKCU "${DESINST}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "${DESINST}" "UninstallString" '"$INSTDIR\desinstalar.exe"'
  WriteRegDWORD HKCU "${DESINST}" "NoModify" 1
  WriteRegDWORD HKCU "${DESINST}" "NoRepair" 1
SectionEnd

Section "Acceso directo en el escritorio" SecEscritorio
  CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\bin\sftp-commander.exe" "" "$INSTDIR\sftp-commander.ico"
SectionEnd

Section /o "Abrir los enlaces sftp:// con ${APP}" SecSftp
  ; desmarcado por defecto: puede pisar la asociación de WinSCP u otro cliente
  WriteRegStr HKCU "Software\Classes\sftp" "" "URL:SFTP"
  WriteRegStr HKCU "Software\Classes\sftp" "URL Protocol" ""
  WriteRegStr HKCU "Software\Classes\sftp\DefaultIcon" "" "$INSTDIR\sftp-commander.ico"
  WriteRegStr HKCU "Software\Classes\sftp\shell\open\command" "" '"$INSTDIR\bin\sftp-commander.exe" "%1"'
  WriteRegStr HKCU "Software\sftp-commander" "EnlacesSftp" "1"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} "El programa y todo lo que necesita (GTK y libssh incluidos)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecEscritorio} "Un acceso directo en el escritorio."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecSftp} "Los enlaces sftp:// del navegador se abren con ${APP}."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  Delete "$SMPROGRAMS\${APP}\${APP}.lnk"
  Delete "$SMPROGRAMS\${APP}\Desinstalar ${APP}.lnk"
  RMDir "$SMPROGRAMS\${APP}"
  Delete "$DESKTOP\${APP}.lnk"
  ; solo se quita la asociación sftp:// si la puso este instalador
  ReadRegStr $0 HKCU "Software\sftp-commander" "EnlacesSftp"
  StrCmp $0 "1" 0 +2
    DeleteRegKey HKCU "Software\Classes\sftp"
  DeleteRegKey HKCU "${DESINST}"
  DeleteRegKey HKCU "Software\sftp-commander"
  ; los servidores guardados (%LOCALAPPDATA%\sftp-commander) y las contraseñas se conservan
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\lib"
  RMDir /r "$INSTDIR\share"
  RMDir /r "$INSTDIR\docs"
  Delete "$INSTDIR\*.md"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\sftp-commander.ico"
  Delete "$INSTDIR\desinstalar.exe"
  RMDir "$INSTDIR"
SectionEnd
