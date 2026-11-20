# Observe the hosted parent-job boundary without changing its policy or native test results.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $IsWindows -or $env:GITHUB_ACTIONS -ne 'true') {
    throw 'This observation is restricted to the Windows Actions test job'
}

$nativeSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public static class MirandaNativeCreationObservation {
    [StructLayout(LayoutKind.Sequential)]
    private struct StartupInfo {
        public uint Size;
        public IntPtr Reserved, Desktop, Title;
        public uint X, Y, Width, Height, Columns, Rows, Fill, Flags;
        public ushort Show, ReservedSize;
        public IntPtr ReservedData, Input, Output, Error;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct StartupInfoEx {
        public StartupInfo Startup;
        public IntPtr Attributes;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInfo {
        public IntPtr Process, Thread;
        public uint ProcessId, ThreadId;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct BasicLimits {
        public long ProcessTime, JobTime;
        public uint Flags;
        public UIntPtr MinimumWorkingSet, MaximumWorkingSet;
        public uint ActiveProcesses;
        public UIntPtr Affinity;
        public uint Priority, Scheduling;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct IoCounters {
        public ulong ReadOperations, WriteOperations, OtherOperations, ReadBytes, WriteBytes, OtherBytes;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct ExtendedLimits {
        public BasicLimits Basic;
        public IoCounters Io;
        public UIntPtr ProcessMemory, JobMemory, PeakProcessMemory, PeakJobMemory;
    }
    public sealed class Case {
        public bool RequestBreakaway, Created, CleanupComplete;
        public uint Flags, ProcessId;
        public int CreationError, TerminationError;
        public uint WaitResult;
    }
    public sealed class Observation {
        public bool MeasurementOnly = true;
        public bool ProductAcceptance = false;
        public bool InitialThreadResumed = false;
        public bool MembershipObserved, InJob, LimitsObserved, UiObserved;
        public int MembershipError, LimitsError, UiError;
        public uint ParentLimits, ParentUiRestrictions;
        public Case[] Cases = new Case[2];
        public bool CleanupComplete, ComparisonComplete;
    }

    public static Observation LastObservation { get; private set; }

    [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
    private static extern IntPtr CreateJobObjectW(IntPtr attributes, IntPtr name);
    [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
    private static extern bool SetInformationJobObject(IntPtr job, int kind, ref ExtendedLimits limits, uint length);
    [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
    private static extern bool QueryInformationJobObject(IntPtr job, int kind, ref ExtendedLimits limits, uint length, IntPtr returned);
    [DllImport("kernel32.dll", EntryPoint = "QueryInformationJobObject", SetLastError = true, ExactSpelling = true)]
    private static extern bool QueryUiInformationJobObject(IntPtr job, int kind, out uint restrictions, uint length, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
    private static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool inJob);
    [DllImport("kernel32.dll", ExactSpelling = true)]
    private static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
    private static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, uint flags, ref UIntPtr size);
    [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
    private static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr key, IntPtr value, UIntPtr size, IntPtr previous, IntPtr returned);
    [DllImport("kernel32.dll", ExactSpelling = true)]
    private static extern void DeleteProcThreadAttributeList(IntPtr list);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
    private static extern bool CreateProcessW(string application, StringBuilder command, IntPtr processAttributes, IntPtr threadAttributes, bool inherit, uint flags, IntPtr environment, string directory, ref StartupInfoEx startup, out ProcessInfo process);
    [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
    private static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
    private static extern bool CloseHandle(IntPtr handle);

    private static void Require(bool success) {
        if (!success) throw new Win32Exception(Marshal.GetLastWin32Error());
    }

    public static Observation Run() {
        var result = new Observation();
        LastObservation = result;
        bool inJob;
        result.MembershipObserved = IsProcessInJob(GetCurrentProcess(), IntPtr.Zero, out inJob);
        result.MembershipError = result.MembershipObserved ? 0 : Marshal.GetLastWin32Error();
        result.InJob = result.MembershipObserved && inJob;
        var parent = new ExtendedLimits();
        if (result.InJob) {
            result.LimitsObserved = QueryInformationJobObject(IntPtr.Zero, 9, ref parent, (uint)Marshal.SizeOf(typeof(ExtendedLimits)), IntPtr.Zero);
            result.LimitsError = result.LimitsObserved ? 0 : Marshal.GetLastWin32Error();
            if (result.LimitsObserved) result.ParentLimits = parent.Basic.Flags;
            uint restrictions;
            result.UiObserved = QueryUiInformationJobObject(IntPtr.Zero, 4, out restrictions, 4, IntPtr.Zero);
            result.UiError = result.UiObserved ? 0 : Marshal.GetLastWin32Error();
            if (result.UiObserved) result.ParentUiRestrictions = restrictions;
        }

        IntPtr job = CreateJobObjectW(IntPtr.Zero, IntPtr.Zero);
        if (job == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        IntPtr attributes = IntPtr.Zero, jobValue = IntPtr.Zero;
        bool initialized = false;
        try {
            var ownedLimits = new ExtendedLimits();
            ownedLimits.Basic.Flags = 0x2000; // Kill on closure applies only to this newly created job.
            Require(SetInformationJobObject(job, 9, ref ownedLimits, (uint)Marshal.SizeOf(typeof(ExtendedLimits))));
            UIntPtr size = UIntPtr.Zero;
            bool unexpectedSuccess = InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
            int sizeError = Marshal.GetLastWin32Error();
            if (unexpectedSuccess || sizeError != 122 || size == UIntPtr.Zero) throw new InvalidOperationException("Unexpected attribute-size query result");
            attributes = Marshal.AllocHGlobal(checked((int)size.ToUInt64()));
            Require(InitializeProcThreadAttributeList(attributes, 1, 0, ref size));
            initialized = true;
            jobValue = Marshal.AllocHGlobal(IntPtr.Size);
            Marshal.WriteIntPtr(jobValue, job);
            Require(UpdateProcThreadAttribute(attributes, 0, new IntPtr(0x2000D), jobValue, new UIntPtr((uint)IntPtr.Size), IntPtr.Zero, IntPtr.Zero));

            for (int index = 0; index < 2; ++index) {
                var item = new Case();
                result.Cases[index] = item;
                item.RequestBreakaway = index == 0;
                // Both children stay suspended: this measures creation, not command execution.
                item.Flags = 0x00080000 | 0x00000400 | 0x00000010 | 0x00000004;
                if (item.RequestBreakaway) item.Flags |= 0x01000000;
                var startup = new StartupInfoEx();
                startup.Startup.Size = (uint)Marshal.SizeOf(typeof(StartupInfoEx));
                startup.Startup.Flags = 1; // Hidden console if Windows creates it before initial-thread execution.
                startup.Attributes = attributes;
                ProcessInfo process;
                item.Created = CreateProcessW(System.IO.Path.Combine(Environment.SystemDirectory, "cmd.exe"), new StringBuilder("cmd.exe /D /C exit 0"), IntPtr.Zero, IntPtr.Zero, false, item.Flags, IntPtr.Zero, Environment.SystemDirectory, ref startup, out process);
                item.CreationError = item.Created ? 0 : Marshal.GetLastWin32Error();
                if (!item.Created) {
                    item.CleanupComplete = true;
                    continue;
                }
                item.ProcessId = process.ProcessId;
                try {
                    bool terminated = TerminateProcess(process.Process, 1223);
                    item.TerminationError = terminated ? 0 : Marshal.GetLastWin32Error();
                    item.WaitResult = WaitForSingleObject(process.Process, 2000);
                    item.CleanupComplete = item.WaitResult == 0;
                    if (!item.CleanupComplete) {
                        // Closing only our owned job supplies a second termination fence.
                        Require(CloseHandle(job));
                        job = IntPtr.Zero;
                        item.WaitResult = WaitForSingleObject(process.Process, 2000);
                        item.CleanupComplete = item.WaitResult == 0;
                    }
                } finally {
                    bool threadClosed = CloseHandle(process.Thread);
                    int threadCloseError = threadClosed ? 0 : Marshal.GetLastWin32Error();
                    bool processClosed = CloseHandle(process.Process);
                    int processCloseError = processClosed ? 0 : Marshal.GetLastWin32Error();
                    if (!threadClosed || !processClosed) throw new Win32Exception(threadClosed ? processCloseError : threadCloseError);
                }
                if (!item.CleanupComplete || job == IntPtr.Zero) {
                    result.CleanupComplete = item.CleanupComplete;
                    return result;
                }
            }
            result.CleanupComplete = true;
            result.ComparisonComplete = true;
            return result;
        } finally {
            if (initialized) DeleteProcThreadAttributeList(attributes);
            if (attributes != IntPtr.Zero) Marshal.FreeHGlobal(attributes);
            if (jobValue != IntPtr.Zero) Marshal.FreeHGlobal(jobValue);
            if (job != IntPtr.Zero) Require(CloseHandle(job));
        }
    }
}
'@

Add-Type -TypeDefinition $nativeSource
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try {
    $observation = [MirandaNativeCreationObservation]::Run()
    [pscustomobject]@{
        source = 'hosted_parent_job_creation_comparison'
        current_identity_is_system = $identity.IsSystem
        observation = $observation
        limitation = 'Two suspended current-account children in a new owned job; no application, permission, or streaming acceptance'
    } | ConvertTo-Json -Depth 6
    if (-not $observation.CleanupComplete -or -not $observation.ComparisonComplete) {
        throw 'The bounded diagnostic did not finish safely; native tests were not started'
    }
} catch {
    [pscustomobject]@{
        source = 'hosted_parent_job_creation_comparison'
        complete = $false
        error_type = $_.Exception.GetType().FullName
        observation = [MirandaNativeCreationObservation]::LastObservation
    } | ConvertTo-Json -Depth 6
    throw
} finally {
    $identity.Dispose()
}
