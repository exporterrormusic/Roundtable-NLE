' ROUNDTABLE NLE — Silent launcher (no terminal window)
' Supports both installed layout (exe alongside this script) and
' development tree (exe in build\bin\Release\).
' Launches invisibly via WScript.Shell.Run with window style 0.

Dim fso, shell, targetExe
Set fso = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")

Dim basePath
basePath = fso.GetParentFolderName(WScript.ScriptFullName) & "\"

' A CUDA build cannot report a missing cudart/cuBLAS DLL from main(): the
' Windows loader resolves those imports before main runs.  The generated
' manifest lets this tiny launcher provide an actionable error instead of the
' generic Windows "DLL not found" dialog.  CPU-only emergency builds do not
' have a manifest and intentionally skip this check.
Sub VerifyCudaRuntime(exePath)
    Dim exeDir, manifestPath
    exeDir = fso.GetParentFolderName(exePath)
    manifestPath = fso.BuildPath(exeDir, "roundtable-cuda-runtime.txt")
    If Not fso.FileExists(manifestPath) Then Exit Sub

    Dim manifestFile, line, dllPath, missing
    missing = ""
    Set manifestFile = fso.OpenTextFile(manifestPath, 1, False)
    Do Until manifestFile.AtEndOfStream
        line = Trim(manifestFile.ReadLine)
        If Len(line) > 0 Then
            If Left(line, 1) <> "#" Then
                dllPath = fso.BuildPath(exeDir, line)
                If Not fso.FileExists(dllPath) Then
                    If Len(missing) > 0 Then missing = missing & vbCrLf
                    missing = missing & "  " & line
                End If
            End If
        End If
    Loop
    manifestFile.Close

    If Len(missing) > 0 Then
        MsgBox "ROUNDTABLE's bundled CUDA runtime is incomplete." & vbCrLf & vbCrLf & _
               "Missing beside roundtable.exe:" & vbCrLf & missing & vbCrLf & vbCrLf & _
               "Reinstall ROUNDTABLE. Installing the CUDA Toolkit is not required on this PC.", _
               16, "ROUNDTABLE - CUDA Runtime Missing"
        WScript.Quit 2
    End If

    Dim systemRoot, system32, driverPath
    systemRoot = shell.ExpandEnvironmentStrings("%SystemRoot%")
    system32 = fso.BuildPath(systemRoot, "System32")
    driverPath = fso.BuildPath(system32, "nvcuda.dll")
    If Not fso.FileExists(driverPath) Then
        MsgBox "The NVIDIA CUDA display-driver component nvcuda.dll was not found." & vbCrLf & vbCrLf & _
               "The CUDA runtime is bundled with ROUNDTABLE, but an NVIDIA display driver is still required. " & _
               "Install or update the NVIDIA driver and try again.", _
               16, "ROUNDTABLE - NVIDIA Driver Missing"
        WScript.Quit 3
    End If
End Sub

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
        ' Prefer Release for normal use; Debug is only for the diag launchers
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

' Validate loader-time CUDA dependencies before starting the GUI process.
VerifyCudaRuntime targetExe

' Set working directory to the exe's directory
shell.CurrentDirectory = fso.GetParentFolderName(targetExe)

' Launch with window style 0 = hidden, bWaitOnReturn = False (async)
shell.Run """" & targetExe & """", 0, False
