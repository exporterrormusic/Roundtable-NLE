' ROUNDTABLE NLE — Silent launcher (no terminal window)
' Supports both installed layout (exe alongside this script) and
' development tree (exe in build\bin\Release\).
' Launches invisibly via WScript.Shell.Run with window style 0.

Dim fso, shell, targetExe
Set fso = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")

Dim basePath
basePath = fso.GetParentFolderName(WScript.ScriptFullName) & "\"

' Prefer exe alongside the script (installed layout)
Dim installedExe
installedExe = basePath & "roundtable.exe"
If fso.FileExists(installedExe) Then
    targetExe = installedExe
Else
    ' Fall back to development tree layout
    Dim releaseExe, debugExe
    releaseExe = basePath & "build\bin\Release\roundtable.exe"
    debugExe   = basePath & "build\bin\Debug\roundtable.exe"

    Dim hasRelease, hasDebug
    hasRelease = fso.FileExists(releaseExe)
    hasDebug   = fso.FileExists(debugExe)

    If hasRelease And hasDebug Then
        ' Prefer Release for normal use; Debug is only for launch_debug.bat
        targetExe = releaseExe
    ElseIf hasRelease Then
        targetExe = releaseExe
    ElseIf hasDebug Then
        targetExe = debugExe
    Else
        MsgBox "ROUNDTABLE executable not found." & vbCrLf & vbCrLf & _
               "Expected: " & releaseExe & vbCrLf & "or: " & debugExe, _
               48, "ROUNDTABLE — Launch Error"
        WScript.Quit 1
    End If
End If

' Set working directory to the exe's directory
shell.CurrentDirectory = fso.GetParentFolderName(targetExe)

' Launch with window style 0 = hidden, bWaitOnReturn = False (async)
shell.Run """" & targetExe & """", 0, False
