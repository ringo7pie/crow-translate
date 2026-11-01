// SPDX-FileCopyrightText: 2026 ringo7pie
// SPDX-License-Identifier: GPL-3.0-or-later
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

// Own the setup worker and every pip/converter child from before it starts.
// Closing/cancelling the progress window cannot leave a download or model
// conversion running in the background.
public sealed class CrowSetupJob : IDisposable {
    [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr window,int command);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    public static void ShowSetupWindow(IntPtr window) {
        // Inno's runhidden / Start-Process Hidden applies SW_HIDE to the first
        // window even when WinForms believes it is visible. Explicitly show
        // only the setup form, keeping the PowerShell console hidden.
        ShowWindow(window,5); SetForegroundWindow(window);
    }
    [StructLayout(LayoutKind.Sequential)] struct BasicLimits {
        public long ProcessTime, JobTime;
        public uint Flags;
        public UIntPtr MinWorkingSet, MaxWorkingSet;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }
    [StructLayout(LayoutKind.Sequential)] struct IoCounters { public ulong A, B, C, D, E, F; }
    [StructLayout(LayoutKind.Sequential)] struct ExtendedLimits {
        public BasicLimits Basic; public IoCounters Io;
        public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
    }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] struct StartupInfo {
        public uint cb; public string reserved, desktop, title;
        public uint x,y,width,height,xChars,yChars,fill,flags;
        public ushort show,reservedSize; public IntPtr reservedPointer, input, output, error;
    }
    [StructLayout(LayoutKind.Sequential)] struct ProcessInfo { public IntPtr Process, Thread; public uint Pid, Tid; }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr a,string n);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetInformationJobObject(IntPtr j,int c,ref ExtendedLimits i,uint n);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr j,IntPtr p);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcess(string app,StringBuilder cmd,IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string dir,ref StartupInfo si,out ProcessInfo pi);
    [DllImport("kernel32.dll")] static extern uint ResumeThread(IntPtr t);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("kernel32.dll")] static extern bool TerminateJobObject(IntPtr h,uint code);
    [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr h,uint code);
    [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h,uint ms);
    [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr h,out uint code);
    IntPtr job, process;
    public uint ProcessId { get; private set; }
    public bool IsRunning { get { return process != IntPtr.Zero && WaitForSingleObject(process,0)==258; } }
    public uint ExitCode { get { uint code; GetExitCodeProcess(process,out code); return code; } }
    public static string Quote(string text) {
        var result=new StringBuilder("\""); int slashes=0;
        foreach(char c in text) {
            if(c=='\\') { slashes++; continue; }
            result.Append('\\', c=='"' ? slashes*2+1 : slashes); result.Append(c); slashes=0;
        }
        result.Append('\\',slashes*2); result.Append('"'); return result.ToString();
    }
    public CrowSetupJob(string executable,string[] args,string directory) {
        job=CreateJobObject(IntPtr.Zero,null);
        if(job==IntPtr.Zero) throw new Win32Exception();
        var limits=new ExtendedLimits(); limits.Basic.Flags=0x2000;
        if(!SetInformationJobObject(job,9,ref limits,(uint)Marshal.SizeOf(limits))) { Dispose(); throw new Win32Exception(); }
        var command=new StringBuilder(Quote(executable)); foreach(var arg in args) command.Append(' ').Append(Quote(arg));
        var info=new StartupInfo(); info.cb=(uint)Marshal.SizeOf(info);
        ProcessInfo created;
        if(!CreateProcess(executable,command,IntPtr.Zero,IntPtr.Zero,false,0x08000004,IntPtr.Zero,directory,ref info,out created)) { Dispose(); throw new Win32Exception(); }
        process=created.Process; ProcessId=created.Pid;
        try {
            if(!AssignProcessToJobObject(job,process)) { TerminateProcess(process,1); throw new Win32Exception(); }
            if(ResumeThread(created.Thread)==0xffffffff) throw new Win32Exception();
        } catch { Dispose(); throw; } finally { CloseHandle(created.Thread); }
    }
    public void Dispose() {
        if(job!=IntPtr.Zero) { TerminateJobObject(job,1223); CloseHandle(job); job=IntPtr.Zero; }
        if(process!=IntPtr.Zero) { CloseHandle(process); process=IntPtr.Zero; }
    }
}
