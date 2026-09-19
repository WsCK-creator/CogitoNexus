@echo off
setlocal EnableExtensions

rem Ten plik tworzy skroty do CogitoNexus.bat:
rem  - lokalnie w tym folderze (dist)
rem  - na Pulpicie Wszystkich Uzytkownikow
rem  - w Menu Start (dzieki czemu aplikacja jest widoczna w wyszukiwarce
rem    na pasku Windows)
rem
rem Uruchom go RECZNIE z tego folderu (dist), najlepiej po zbudowaniu projektu.
rem Utworzenie skrotu na Pulpicie Wszystkich Uzytkownikow oraz w Menu Start
rem wymaga uprawnien administratora - jesli ich nie masz, kliknij ten plik
rem prawym przyciskiem myszy i wybierz "Uruchom jako administrator".

set "DIST=%~dp0"
set "TARGET_BAT=%DIST%CogitoNexus.bat"
set "ICON=%DIST%CogitoNexus.ico"

if not exist "%TARGET_BAT%" (
    echo Nie znaleziono "%TARGET_BAT%" - uruchom najpierw build ^(build_cogito-nexus.ps1^).
    goto :end
)

if not exist "%ICON%" (
    echo Uwaga: brak pliku ikony "%ICON%", skroty beda bez wlasnej ikony.
    set "ICON=%TARGET_BAT%"
)

set "DESKTOP_ALL=%PUBLIC%\Desktop"
set "STARTMENU_ALL=%ALLUSERSPROFILE%\Microsoft\Windows\Start Menu\Programs"
set "VBS=%TEMP%\cogito_skroty_%RANDOM%.vbs"

> "%VBS%" (
    echo Set oWS = WScript.CreateObject^("WScript.Shell"^)
    echo Function MakeShortcut^(lnkPath, opis^)
    echo   On Error Resume Next
    echo   Err.Clear
    echo   Set oLink = oWS.CreateShortcut^(lnkPath^)
    echo   oLink.TargetPath = "%TARGET_BAT%"
    echo   oLink.WorkingDirectory = "%DIST%"
    echo   oLink.IconLocation = "%ICON%"
    echo   oLink.Save
    echo   If Err.Number ^<^> 0 Then
    echo     WScript.Echo "BLAD: " ^& opis ^& " -^> " ^& lnkPath
    echo     WScript.Echo "  Brak uprawnien. Uruchom ten plik jako Administrator ^(prawy przycisk myszy -^> Uruchom jako administrator^)."
    echo   Else
    echo     WScript.Echo "OK: " ^& opis ^& " -^> " ^& lnkPath
    echo   End If
    echo End Function
    echo MakeShortcut "%DIST%CogitoNexus.lnk", "folder dist"
    echo MakeShortcut "%DESKTOP_ALL%\CogitoNexus.lnk", "Pulpit Wszystkich Uzytkownikow"
    echo MakeShortcut "%STARTMENU_ALL%\CogitoNexus.lnk", "Menu Start"
)

cscript //nologo "%VBS%"
del "%VBS%" >nul 2>nul

:end
echo.
echo --- Gotowe ---
pause
