@echo off
rem The Sims for PS Vita: copies the game folders it needs from your own install of
rem The Sims Legacy Collection to the Vita's memory card (ux0:data/TheSims). It only reads
rem from the install.
rem
rem 1. On the Vita, start VitaShell and press SELECT: the memory card shows up as a drive.
rem 2. Run:  copy_to_vita.cmd <drive letter> "<install folder>"
rem    e.g.  copy_to_vita.cmd E "C:\Program Files (x86)\Steam\steamapps\common\The Sims Legacy Collection"
rem    (Steam: right-click the game, Manage, Browse local files, to find the folder.)
setlocal
set DRIVE=%~1
set GAME=%~2
if "%DRIVE%"=="" (
    echo usage: copy_to_vita.cmd ^<Vita drive letter^> "^<install folder^>"
    exit /b 1
)
set DRIVE=%DRIVE::=%
if "%GAME%"=="" set GAME=C:\Program Files (x86)\Steam\steamapps\common\The Sims Legacy Collection
if not exist "%DRIVE%:\" (
    echo Drive %DRIVE%: not found. Start VitaShell, press SELECT for USB mode, then run this again.
    exit /b 1
)
if not exist "%GAME%\GameData\UIText.iff" (
    echo The game was not found in "%GAME%".
    echo Give the install folder as the second argument, in quotes.
    exit /b 1
)
set DEST=%DRIVE%:\data\TheSims
echo Copying from "%GAME%" to %DEST% (about 3 GB: this takes a while)...
for %%F in (GameData SoundData Music UIGraphics ExpansionShared ExpansionPack ExpansionPack2 ExpansionPack3 ExpansionPack4 ExpansionPack5 ExpansionPack6 ExpansionPack7 Deluxe Downloads UserData UserData2 UserData3 UserData4 UserData5 UserData6 UserData7 UserData8) do (
    if exist "%GAME%\%%F" (
        echo   %%F
        robocopy "%GAME%\%%F" "%DEST%\%%F" /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 >nul
        if errorlevel 8 (
            echo Copying %%F failed: is the memory card full?
            exit /b 1
        )
    )
)
echo Done. Leave USB mode in VitaShell, then start the game from its bubble.
