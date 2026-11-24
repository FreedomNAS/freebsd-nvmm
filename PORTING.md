# Porting di NVMM su FreeBSD 15.1 (amd64, Intel VMX)

Stato: **analisi, fase 1**. Nessuna logica di virtualizzazione scritta.

Sorgenti consultati (clonati in `refs/`, ignorata da git):

| Albero | Ref | Commit |
|---|---|---|
| NetBSD `sys/dev/nvmm`, `lib/libnvmm` | trunk | `6af31c31` (2026-11-23) |
| DragonFly `sys/dev/virtual/nvmm`, `lib/libnvmm` | master | `9f0e625c` (2026-11-23) |
| FreeBSD (solo API) | `releng/15.1` (15.1-RELEASE-p7) | `bb03631f` |
| QEMU `target/i386/nvmm`, `meson.build` | master | `2bfbe39d` |

---

## 1. Struttura del codice NVMM

NVMM è diviso in tre strati: un **frontend MI** (machine independent) che
gestisce macchine, vCPU, memoria e ioctl; un **backend MD** x86 (VMX per
Intel, SVM per AMD) che sa solo eseguire una vCPU e tradurre le uscite; una
**libreria userland** (libnvmm) che fa da ABI stabile e contiene
l'emulatore di istruzioni per I/O e MMIO. Il kernel non emula dispositivi:
li emula QEMU.

### 1.1 Frontend MI (kernel)

| File (DragonFly) | Righe | Ruolo |
|---|---:|---|
| `nvmm.h` | 101 | ABI pubblica: `nvmm_machid_t`, `nvmm_cpuid_t`, `struct nvmm_capability`, `struct nvmm_comm_page`, `NVMM_KERN_VERSION`. Incluso anche da userland. |
| `nvmm_ioctl.h` | 157 | Strutture e numeri degli ioctl (`_IOR/_IOW/_IOWR('N', n, …)`): capability, machine create/destroy/configure, vcpu create/destroy/configure/setstate/getstate/inject/run, hva/gpa map/unmap, ctl. |
| `nvmm_internal.h` | 147 | Strutture kernel: `nvmm_owner` (chi ha aperto il device), `nvmm_cpu`, `nvmm_machine` (vmspace guest, hmapping, vCPU), e soprattutto `struct nvmm_impl`: la tabella di funzioni che ogni backend implementa. Limiti: 128 macchine, 128 vCPU, 32 hmapping. |
| `nvmm.c` | 1022 | Il frontend vero: tabella statica delle macchine con un rwlock ciascuna, mutex per vCPU, dispatch degli ioctl, controlli di proprietà (owner/pid), gestione memoria (hva_map crea un oggetto VM e lo mappa nel processo, gpa_map mappa lo stesso oggetto nello spazio guest), ciclo `nvmm_do_vcpu_run` (chiama il backend, risolve i fault di memoria guest, torna in userland se c'è un segnale o un reschedule), `nvmm_ident/init/fini`, `nvmm_ctl` per nvmmctl(8). Nessuna chiamata diretta al kernel ospite: passa tutto da `os_*`. |
| `nvmm_os.h` | 403 | Strato di astrazione del sistema operativo: tipi (`os_vmspace_t`, `os_vmobj_t`, `os_rwl_t`, `os_mtx_t`, `os_cpu_t`, `os_cpuset_t`), macro per lock, malloc, atomics, CPU corrente, cpuset, preemption, IPI, copyin/out, e i prototipi delle funzioni `os_*` da implementare in un file per sistema. |
| `nvmm_dragonfly.c` | 437 | Implementazione `os_*` per DragonFly (vmspace, vm_object, mappature, pagine fisiche, memoria contigua) + glue del device (`make_dev`, cdevpriv, ioctl) + `DECLARE_MODULE`. **È il file che per FreeBSD diventa `nvmm_freebsd.c`.** |
| `nvmm_netbsd.c` | 474 | Lo stesso per NetBSD (tenuto nell'albero DragonFly per restare allineati). |

### 1.2 Backend MD x86

| File | Righe | Ruolo |
|---|---:|---|
| `x86/nvmm_x86.h` | 682 | ABI x86 pubblica: stato della vCPU (segmenti, GPR, CR, DR, MSR, stato interrupt, FPU x87/SSE), motivi di uscita (`NVMM_VCPU_EXIT_*`: memoria, I/O, MSR, HLT, CPUID, …), eventi da iniettare, configurazioni (CPUID, TPR), `nvmm_cap_md`. |
| `x86/nvmm_x86_internal.h` | 527 | Solo DragonFly: costanti CPUID/MSR/CR e primitive CPU per sistema (`x86_get_cr*`, debug register, FPU save/restore, `xgetbv/xsetbv`). In NetBSD queste cose vengono da `<x86/…>`. |
| `x86/nvmm_x86.c` | 507 | Codice comune VMX/SVM: maschere CPUID esposte al guest, validazione PAT, dimensione XSAVE, sanitizzazione dello stato. |
| `x86/nvmm_x86_vmx.c` | 3753 | Backend Intel. `vmx_ident` (legge `IA32_VMX_*`: controlli pin/proc/entry/exit, EPT 4 livelli, INVEPT, INVVPID, unrestricted guest), `vmx_init` (VMXON su ogni CPU tramite IPI `vmx_change_cpu`), allocazione VPID ("ASID"), creazione VMCS e MSR bitmap/I/O bitmap per vCPU, gestori di uscita (CPUID, HLT, I/O, RDMSR/WRMSR, CR, XSETBV, EPT violation → uscita memoria), iniezione eventi, get/set stato via comm page, generazioni TLB (guest con INVVPID, host con INVEPT), ciclo `vmx_vcpu_run`, mitigazione L1TF (L1D flush nella MSR list d'ingresso). |
| `x86/nvmm_x86_vmxfunc.S` | 205 | `vmx_vmxon/vmxoff`, `vmx_vmlaunch/vmresume` con salvataggio/ripristino GPR host e guest, LDT host. Non dipende da offset di strutture del kernel (DragonFly genera comunque `assym.h`, a noi non serve). |
| `x86/nvmm_x86_svm.c`, `x86/nvmm_x86_svmfunc.S` | 2993 + 179 | Backend AMD. Fuori scope fino all'ultima fase. |

### 1.3 libnvmm (userland)

| File | Righe (DF) | Ruolo |
|---|---:|---|
| `nvmm.h` | 129 | API pubblica: `struct nvmm_machine`, `nvmm_vcpu`, `nvmm_io`, `nvmm_mem`, callback di assist, `NVMM_USER_VERSION`. È l'header che usa QEMU. |
| `libnvmm.c` | 576 | Apertura di `/dev/nvmm` (con `O_CLOEXEC`), wrapper degli ioctl, lista delle aree hva↔gpa per tradurre indirizzi, gestione della comm page (stato vCPU senza copie), `nvmm_vcpu_run`. |
| `libnvmm_x86.c` | 3819 | Assist per I/O e MMIO: decoder ed emulatore di istruzioni x86 (MOV, MOVS, STOS, INS/OUTS, aritmetiche su memoria…), page walk GVA→GPA, controlli di segmento. Il pezzo più grande ma puro userland. |
| `libnvmm.3` | 801 | Manuale. |

---

## 2. NetBSD vs DragonFly: cosa ha astratto DragonFly e come

Le due versioni sono lo stesso codice di Maxime Villard, ma divise in due
linee che oggi si scambiano patch (a febbraio 2026 NetBSD ha sincronizzato i
bit CPUID "con DragonFly"; a novembre 2026 DragonFly ha fatto un giro di
"Improve OS independence"). Le differenze che contano:

1. **Strato di sistema.** NetBSD chiama direttamente UVM, `kmem`, `xcall`,
   le funzioni FPU x86 e autoconf. DragonFly ha messo tutto dietro
   `nvmm_os.h` + `nvmm_<os>.c` (memoria e mappature `os_vmspace_*`,
   `os_vmobj_*`, `os_pagemem_*`, `os_pa_*`, `os_contigpa_*`; lock; cpuset;
   IPI; preemption; `os_return_needed`) e `x86/nvmm_x86_internal.h` per le
   primitive CPU. Il frontend e i backend contengono pochissimi `#ifdef`
   (in `vmx_vcpu_run`, `vmx_machine_create`, `vmx_ident`). Aggiungere un
   terzo sistema significa aggiungere un ramo `__FreeBSD__` e un file.
2. **Comm page (la pagina condivisa con lo stato della vCPU).** NetBSD:
   l'utente fa `mmap()` su `/dev/nvmm` all'offset `NVMM_COMM_OFF(machid,
   cpuid)` (serve un `fo_mmap` per file). DragonFly: è il kernel che mappa
   l'oggetto sia nel kernel sia nello spazio del processo durante
   `vcpu_create` e restituisce l'indirizzo in `args->comm`; nessun mmap sul
   device.
3. **ABI incompatibili tra loro.** NetBSD: kernel v2, libnvmm v2, con
   `comm->stop` e `nvmm_vcpu_stop()` (uscita senza race da `vcpu_run`,
   usata da QEMU se `NVMM_USER_VERSION >= 2`), più
   `NVMM_CAP_ARCH_VCPU_CONF_XCR0_MASK` e lo stato XSAVE esteso (luglio
   2026). DragonFly: kernel v3 (`comm_size` in `nvmm_capability`),
   libnvmm v1, niente `stop`. Una libnvmm di un sistema non parla con il
   kernel dell'altro.
4. **FPU.** NetBSD: `fpu_kern_enter()` + `fpu_area_save/restore`.
   DragonFly: `kernel_fpu_begin()` + `fpusave/fpurstor` + gestione di
   `CR0.TS` a mano. DragonFly ha anche reso autonome le strutture FPU
   dell'ABI (non includono più `<x86/cpu_extended_state.h>`).
5. **pmap EPT e TLB host.** Entrambi i sistemi hanno **modificato il
   proprio pmap** per NVMM: `pmap_ept_transform()` trasforma il pmap di un
   vmspace in un pmap EPT. NetBSD aggiunge una callback `pm_tlb_flush` che
   incrementa una generazione e manda un finto TLB shootdown a tutte le CPU;
   DragonFly usa `pm_invgen`, `pmap_add_cpu()` e un flag per-CPU
   (`RQF_HVM_MASK`, `hvm_break_wanted()`) aggiunto al kernel apposta.
   **FreeBSD non ha bisogno di modifiche al kernel**: il pmap ha già il tipo
   `PT_EPT` per bhyve (vedi §3).
6. **Commit degli eventi.** DragonFly sposta `vmx_vcpu_event_commit()` dopo
   il `cli` e, se ci sono eventi host pendenti, esce con `ERESTART` per non
   perdere l'evento iniettato. NetBSD lo fa prima.
7. **Preemption.** NetBSD: `kpreempt_disable()` + `curlwp_bind()`.
   DragonFly: `crit_enter()`, niente bind (i thread kernel non migrano).
8. **Suspend/resume.** Solo NetBSD (2022, pmf). DragonFly non lo gestisce.
9. **Device.** NetBSD: pseudo-device autoconf + `fd_clone` con fileops
   proprie. DragonFly: `make_dev` + `devfs_set_cdevpriv`; richiede
   `O_CLOEXEC` all'apertura; `O_WRONLY` = nvmmctl.
10. **Limiti.** RAM guest massima 128 GiB in NetBSD, 127 TiB in DragonFly.
11. **Correzioni recenti non ancora incrociate.** NetBSD 2026: race sul bit
    `stop`, leak nella distruzione della macchina, lettura di
    `VMCS_EXIT_REASON` dopo un `vmlaunch` fallito, #CP, XSAVE esteso.
    DragonFly novembre 2026: CR0.NE in VMX, filtraggio e reiniezione degli
    eventi, `xcr0_valid/cr4_valid/efer_valid` dinamici, CPUID, XSETBV. Vanno
    verificate una per una sulla base scelta.

### Scelta della base: DragonFly

Motivi:

- ha già lo strato `nvmm_os.h`: il porting è un ramo `__FreeBSD__` più
  `nvmm_freebsd.c`, non una riscrittura delle chiamate UVM sparse in
  `nvmm.c`;
- il VM di FreeBSD e quello di DragonFly hanno lo stesso antenato
  (`vm_map`, `vm_object`, `vm_page`, `vmspace`): `os_vmobj_*` si traduce
  quasi riga per riga, mentre UVM di NetBSD ha un modello diverso;
- la comm page mappata dal kernel evita di scrivere un `d_mmap_single`
  per-fd (fattibile su FreeBSD, ma una superficie in più);
- è la linea su cui l'autore lavora attivamente per l'indipendenza dal
  sistema operativo.

La questione dell'ABI (v3 DragonFly o v2 NetBSD) è aperta: vedi decisione
**D1** in fondo.

---

## 3. Mappatura delle API kernel

Legenda: NB = NetBSD, DF = DragonFly, FB = FreeBSD 15.1. I nomi FreeBSD
sono quelli verificati in `refs/freebsd/sys` e, dove c'è un precedente, in
bhyve (`sys/amd64/vmm`, `sys/dev/vmm`).

### 3.1 Memoria guest

| Funzione NVMM | NB | DF | FB 15.1 |
|---|---|---|---|
| spazio di indirizzi guest | `uvmspace_alloc` + `pmap_ept_transform` (pmap patchato) | `vmspace_alloc` + `pmap_ept_transform(pmap, PMAP_EMULATE_AD_BITS?)` (pmap patchato) | `vmspace_alloc(min, max, pinit)` con `pinit` = `pmap_pinit_type(pmap, PT_EPT, flags)`, esattamente come `ept_vmspace_alloc()` di bhyve; `vmspace_free()` |
| flag del pmap EPT | `pmap_ept_has_ad` globale | flag in `pmap_ept_transform` | `pm_flags`: `PMAP_PDE_SUPERPAGE`, `PMAP_EMULATE_AD_BITS` (Ivy Bridge non ha i bit A/D nell'EPT, Haswell sì), `PMAP_SUPPORTS_EXEC_ONLY`, e il vettore IPI in `PMAP_NESTED_IPIMASK` |
| oggetto di memoria (hva/gpa) | `uao_create` | `default_pager_alloc` | `vm_object_allocate(OBJT_SWAP, atop(size))` (o `OBJT_PHYS` se cablata, vedi **D2**); `vm_object_reference` / `vm_object_deallocate` |
| mappare l'oggetto nel processo (hva) e nel guest (gpa) | `uvm_map` | `vm_map_insert` + `vm_map_inherit` + `vm_map_kernel_wiring` | `vm_map_fixed()` / `vm_map_find()` con `MAP_INHERIT_SHARE` o `vm_map_inherit()`; `vm_map_wire()` se cablata; `vm_map_remove()` |
| fault su EPT violation | `uvm_fault` | `vm_fault(map, va, prot, flags)` | `pmap_emulate_accessed_dirty()` poi `vm_fault(map, gpa, ftype, VM_FAULT_NORMAL, NULL)` (come `vm_handle_paging` di bhyve) |
| invalidazione TLB host dopo modifiche al pmap guest | callback `pm_tlb_flush` → generazione per macchina + kick a tutte le CPU | `pm_invgen` + `pmap_add_cpu` + `RQF_HVM_MASK` | **Nessuna callback**: il pmap FreeBSD incrementa `pm_eptgen`, manda l'IPI `PMAP_NESTED_IPIMASK` alle CPU in `pm_active` e aspetta con SMR (`pm_eptsmr`). Il backend deve fare quello che fa `vmx_run` di bhyve: `CPU_SET_ATOMIC(cpu, &pm_active)`, `smr_enter(pm_eptsmr)`, confronto con la generazione vista per CPU, `INVEPT` se diversa, `smr_exit` dopo il VM exit. Sostituisce `mach_htlb_gen`/`htlb_want_flush` di NVMM. |
| pagine kernel allineate (VMCS, bitmap) | `uvm_km_alloc` | `kmem_alloc(kernel_map)` | `kmem_malloc(size, M_WAITOK \| M_ZERO)` / `kmem_free(ptr, size)` |
| una pagina fisica azzerata | `uvm_pagealloc` | `vm_page_alloczwq` | `vm_page_alloc_noobj(VM_ALLOC_WIRED \| VM_ALLOC_ZERO \| VM_ALLOC_WAITOK)` + `VM_PAGE_TO_PHYS`; libera con `vm_page_unwire_noq` + `vm_page_free` |
| memoria fisicamente contigua | `uvm_pglistalloc` | `contigmalloc` / `contigfree` | `kmem_alloc_contig(size, M_WAITOK \| M_ZERO, 0, ~0, PAGE_SIZE, 0, VM_MEMATTR_WRITE_BACK)` / `kmem_free`; indirizzo fisico con `vtophys()` |
| indirizzo fisico della PML4 guest | `pm_pdirpa[0]` | `vtophys(pm_pml4)` | `vtophys(pmap->pm_pmltop)` (radice EPT; bhyve la passa a `eptp()`) |

### 3.2 Allocazione, lock, atomics

| | NB | DF | FB 15.1 |
|---|---|---|---|
| malloc | `kmem_alloc/kmem_zalloc` | `kmalloc(M_NVMM)` | `malloc(9)`, `MALLOC_DEFINE(M_NVMM, …)`, `M_WAITOK \| M_ZERO` |
| rwlock macchina | `krwlock_t` | `lockmgr` | `sx(9)` (`sx_slock/sx_xlock/sx_xunlock`, `sx_xlocked`). Non `rwlock(9)`: chi lo tiene dorme (malloc `M_WAITOK`, `vm_map_*`). |
| mutex vCPU | `kmutex_t` | `lockmgr` esclusivo | `sx(9)` esclusivo. Non `mtx(9)`: è tenuto per tutta `vcpu_run`, che può dormire in `vm_fault`. |
| atomics | `atomic_inc_uint`… | `atomic_add_int`… | `atomic_add_int`, `atomic_subtract_int`, `atomic_load_int`, `atomic_add_64` (`<machine/atomic.h>`) |
| assert | `KASSERT` | `KKASSERT` | `KASSERT(expr, (msg))` (sintassi diversa: serve un wrapper), `sx_assert(SA_XLOCKED)` |
| printf | `printf` | `kprintf` | `printf` |
| tempo | `time_second` | `time_second` | `time_second` |

### 3.3 CPU, per-CPU, preemption, IPI

| | NB | DF | FB 15.1 |
|---|---|---|---|
| iterare le CPU | `CPU_INFO_FOREACH` | `globaldata_find(i)` | `CPU_FOREACH(i)` + `pcpu_find(i)`; `mp_ncpus`, `mp_maxid`, `MAXCPU` |
| CPU corrente | `cpu_number()` | `mycpuid` | `curcpu` / `PCPU_GET(cpuid)` |
| TSS, GDT, GS base, IDT host (per la VMCS) | `ci_tss`, `ci_gdt`… | `gd_prvspace->common_tss`… | `PCPU_GET(tssp)`, `*PCPU_PTR(gdt)`, `get_pcpu()`, `r_idt.rd_base`, selettore TR `GSEL(GPROC0_SEL, SEL_KPL)` (le stesse di `vmm_host.h`) |
| cpuset | `kcpuset_t` | `cpumask_t` | `cpuset_t`, `CPU_ISSET`, `CPU_SET_ATOMIC`, `CPU_CLR_ATOMIC`, `all_cpus` |
| niente preemption | `kpreempt_disable` | `crit_enter` | `critical_enter()` / `critical_exit()` (con interrupt hardware sempre possibili) |
| legare il thread alla CPU | `curlwp_bind` | non serve | `sched_pin()` / `sched_unpin()` |
| "devo tornare in userland?" | `preempt_needed()`, `LW_USERRET` | `hvm_break_wanted()`, `LWP_MP_URETMASK` | `td->td_ast != 0 \|\| td->td_owepreempt` (identico a `vcpu_should_yield()` di bhyve) |
| IPI su una CPU con attesa | `xc_unicast` + `xc_wait` | `lwkt_send_ipiq` + `lwkt_wait_ipiq` | `smp_rendezvous_cpus(set, smp_no_rendezvous_barrier, fn, smp_no_rendezvous_barrier, arg)` |
| IPI su tutte le CPU | `xc_broadcast` | `lwkt_cpusync_simple` | `smp_rendezvous(NULL, fn, NULL, arg)` (usato da bhyve per VMXON/VMXOFF) |
| kick di una vCPU in esecuzione | finto TLB shootdown | gd_reqflags | vettore proprio da `lapic_ipi_alloc(pti ? &IDTVEC(justreturn1_pti) : &IDTVEC(justreturn))`, poi `ipi_cpu()`/`ipi_selected()`; liberarlo con `lapic_ipi_free()` allo scarico |
| resume da S3 (VMXON perso) | pmf | — | puntatore `vmm_resume_p` (uno solo, chiamato da `mp_x86.c`): libero se vmm.ko non c'è. Fase successiva. |

### 3.4 FPU, MSR, registri

| | NB | DF | FB 15.1 |
|---|---|---|---|
| liberare la FPU dal thread host | `fpu_kern_enter()` | `kernel_fpu_begin()` | `fpu_kern_enter(curthread, NULL, FPU_KERN_NOCTX)`: salva lo stato utente nel PCB, entra in sezione critica; `fpu_kern_leave(curthread, NULL)` alla fine. Alternativa bhyve: `fpuexit(curthread)` + `fpu_enable()`/`fpu_disable()`. |
| salvare/caricare la FPU guest | `fpu_area_save/restore(area, mask)` | `fpusave/fpurstor` + CR0.TS | `xsave`/`xrstor` inline con la **maschera della vCPU** (le `fpusave()`/`fpurestore()` di FreeBSD usano la maschera globale `xsave_mask`, non adatta); XCR0 guest con `xsetbv` (già in NVMM) |
| feature XSAVE e MXCSR mask host | `x86_xsave_features`, `x86_fpu_mxcsr_mask` | `npx_xcr0_mask`, `npx_mxcsr_mask` | `xsave_mask` (o `vmm_host_xcr0` calcolato con `rxcr(0)`), `cpu_mxcsr_mask`, `cpu_max_ext_state_size` |
| MSR | `rdmsr/wrmsr` | uguali | `rdmsr`, `wrmsr`, `rdmsr_safe` (`<machine/cpufunc.h>`) |
| CR e DR | `rcr0/lcr0`, `rdr0/ldr0` | `load_cr0`, `load_dr0` | `rcr0/load_cr0`, `rcr4/load_cr4`, `rdr0…rdr7`, `load_dr0…load_dr7` |
| debug register del thread host | `x86_dbregs_save(curlwp)` | `pcb_flags & PCB_DBREGS` | uguale a DF: `curpcb->pcb_flags & PCB_DBREGS`, `pcb_dr0…pcb_dr7` |
| CPUID | `x86_cpuid2` | `cpuid_count` | `cpuid_count(leaf, subleaf, regs[4])`, `do_cpuid`; `cpu_feature2 & CPUID2_VMX`, `cpu_vendor_id == CPU_VENDOR_INTEL` |

### 3.5 Userland, device, modulo

| | NB | DF | FB 15.1 |
|---|---|---|---|
| copyin/copyout | uguali | uguali | `copyin`, `copyout` (gli argomenti degli ioctl `_IOR/_IOW` li copia già il kernel, fino a `IOCPARM_MAX` = 8 KiB; i puntatori annidati come `conf` restano a `copyin`) |
| device node | pseudo-device + `fd_clone` | `make_dev` + `dev_ops` | `make_dev_s(&args, &dev, "nvmm")` con `struct cdevsw { .d_version = D_VERSION, .d_name, .d_open, .d_ioctl }`, `mda_mode = 0640`, gruppo dedicato da decidere |
| stato per file descriptor | file proprio | `devfs_set_cdevpriv(fp, …)` | `devfs_set_cdevpriv(owner, dtor)` / `devfs_get_cdevpriv(&owner)`; il distruttore chiama `nvmm_kill_machines()` |
| `O_CLOEXEC` obbligatorio | sì | sì | `d_open` di FreeBSD non vede `O_CLOEXEC` (diventa un flag del descrittore): il controllo va tolto o spostato in libnvmm |
| comm page nel processo | mmap su `/dev/nvmm` | mappata dal kernel | come DF (`vm_map_find` nel `vmspace` di `curproc`) |
| modulo | `MODULE(MODULE_CLASS_DRIVER, nvmm, …)` | `DECLARE_MODULE` | `DECLARE_MODULE(nvmm, …, SI_SUB_DRIVERS, SI_ORDER_ANY)` + `MODULE_VERSION(nvmm, 1)`; build con `bsd.kmod.mk` |
| vmm.ko presente? | — | — | `MOD_SLOCK` + `module_lookupbyname("vmm")` al `MOD_LOAD` (vmm.ko dichiara `DECLARE_MODULE(vmm, …)` + `MODULE_VERSION(vmm, 1)` in `sys/dev/vmm/vmm_dev.c`). Per il caso inverso vedi **D3**. |

---

## 4. Punti rischiosi (dal più difficile)

1. **Memoria guest sul pmap EPT di FreeBSD.** Il codice NVMM presume una
   callback dal pmap; su FreeBSD bisogna invece adottare il protocollo di
   bhyve (`pm_active`, `pm_eptgen`, SMR, vettore IPI nei flag del pmap). Un
   errore qui significa TLB stantii: il guest vede pagine già liberate
   dall'host, cioè corruzione di memoria dell'host. Su Ivy Bridge (3ª gen)
   l'EPT non ha i bit A/D: serve `PMAP_EMULATE_AD_BITS` e il passaggio da
   `pmap_emulate_accessed_dirty()` prima di `vm_fault()`.
2. **Ciclo `vcpu_run` dentro lo scheduler FreeBSD.** Sezione critica,
   interrupt disabilitati intorno a VM entry, controllo di `td_ast`, segnali
   (QEMU sveglia i thread vCPU con un segnale), semantica di `ERESTART`
   negli ioctl FreeBSD (riavvia la syscall: va bene solo se lo stato è
   coerente), ripristino degli MSR host (`KGSBASE`, `STAR/LSTAR/SFMASK`
   già gestiti da NVMM; da verificare `TSC_AUX`, `PAT`, `EFER`, e i vettori
   PTI: sugli Intel di 3ª/4ª gen FreeBSD attiva PTI per Meltdown).
3. **FPU/XSAVE.** FreeBSD fa lo switch FPU pigro (`fpcurthread`,
   `PCB_KERNFPU`): se il guest scrive la FPU mentre il kernel crede che sia
   del thread host, si corrompe lo stato dei processi utente in modo
   silenzioso. Va provato con un guest che usa AVX e un host che fa lo
   stesso.
4. **Convivenza con altri hypervisor.** Se vmm.ko viene caricato *dopo*
   nvmm.ko, il suo `VMXON` fallisce in silenzio (siamo già in VMX root) e
   allo scarico fa `load_cr4(rcr4() & ~CR4_VMXE)` mentre la CPU è in VMX
   operation: **#GP e panic**. Peggio: con `VMPTRLD` bhyve potrebbe
   eseguire VM dentro il nostro VMXON. VirtualBox (vboxdrv) invece rileva
   da solo `CR4.VMXE` e si rifiuta.
5. **Comm page e memoria mappate nel processo.** Ordine di distruzione tra
   chiusura del descrittore (`cdevpriv` dtor), uscita del processo
   (vmspace già smontato) e `fork()` (mappature da non ereditare). DragonFly
   ha un commento proprio su questo in `nvmm.c:154`.
6. **Scarico del modulo.** Rifiutare con macchine attive, `VMXOFF` e
   `CR4.VMXE` spento su tutte le CPU, liberare vettore IPI, VPID e regioni
   VMXON. Un errore qui è un panic al `kldunload`.
7. **Suspend/resume.** Dopo S3 le CPU escono da VMX: senza hook le VM
   ripartono su una CPU non in VMX operation. Su un NAS il suspend è raro:
   fase successiva.
8. **Correzioni non incrociate tra NetBSD e DragonFly** (§2 punto 11):
   vanno controllate perché alcune sono di sicurezza.
9. **libnvmm e QEMU.** Rischio basso: libnvmm è userland portabile;
   QEMU abilita NVMM solo con `host_os == 'netbsd'` in `meson.build`,
   quindi serve una patch di build più gli `#include` di sistema.

---

## 5. Roadmap

Ogni fase finisce con un modulo che si carica e si scarica senza panic
sulla tua macchina di test, e con un commit per passo logico.

| Fase | Contenuto | Test di accettazione (tuo, su hardware dedicato) |
|---|---|---|
| **1. Scheletro** | `sys/modules/nvmm/Makefile`, `sys/dev/nvmm/`: frontend DragonFly importato, ramo `__FreeBSD__` in `nvmm_os.h`, `nvmm_freebsd.c` con device e modulo; `vmx_ident` reale (VMX, EPT, unrestricted guest, INVEPT/INVVPID) senza VMXON; rifiuto se vmm.ko è presente; `/dev/nvmm` con `NVMM_IOC_CAPABILITY` funzionante e `ENOSYS` per il resto | build pulita; `kldload` su i5 → messaggio "attached"; su CPU senza VMX/EPT/UG → rifiuto con messaggio, niente panic; con vmm.ko caricato → rifiuto; un piccolo programma legge la capability; `kldunload` pulito |
| **2. VMXON per CPU** | `vmx_init/fini`: `IA32_FEATURE_CONTROL`, `CR4.VMXE`, regioni VMXON, `smp_rendezvous`, VMXOFF allo scarico, vettore IPI | load/unload ripetuti 100 volte; `kldload vmm` dopo nvmm rifiutato (secondo **D3**) |
| **3. Macchina e vCPU** | vmspace EPT, VMCS, VPID, MSR/I/O bitmap, comm page nel processo, ioctl create/destroy/configure/setstate/getstate | creazione e distruzione di 128 macchine; uscita del processo senza distruggere → nessun leak (`vmstat -m`, `sysctl vm.stats`) |
| **4. Memoria guest** | `hva_map/unmap`, `gpa_map/unmap`, oggetti VM, fault, protocollo `pm_eptgen` | test che mappa, scrive dall'host e rilegge dal pmap guest; unmap mentre un'altra vCPU gira |
| **5. vcpu_run** | ciclo di esecuzione, uscite, iniezione eventi, FPU, segnali | guest minimale (pochi byte di codice in real mode che fa `out` e `hlt`), poi un kernel Linux con `-kernel` |
| **6. libnvmm** | port di `lib/libnvmm` DragonFly (Makefile `bsd.lib.mk`, header), nvmmctl(8) | i test di libnvmm (`tests/lib/libnvmm` di NetBSD/DragonFly) |
| **7. QEMU `accel=nvmm`** | patch a `meson.build` (`host_os == 'freebsd'`), header, port in `emulators/qemu` locale | FreeBSD e Linux guest con `-accel nvmm`, confronto con TCG; poi l'integrazione nel modulo VM di FreedomNAS |
| **8. SVM** | backend AMD | come le fasi 5–7 su una macchina AMD |

Fasi trasversali, da fare quando servono: suspend/resume, incrocio delle
correzioni NetBSD↔DragonFly, sysctl/statistiche, pacchetto FreeBSD.

---

## 6. Scelte aperte (servono le tue decisioni)

### D1. Quale ABI esporre a userland

| Opzione | Pro | Contro |
|---|---|---|
| **A. ABI DragonFly** (kernel v3, libnvmm v1) — *consigliata ora* | kernel e libnvmm dallo stesso albero, sempre coerenti; zero divergenze da mantenere; QEMU funziona come su DragonFly oggi | QEMU usa il percorso senza `nvmm_vcpu_stop()` (kick via segnale, meno elegante); manca lo stato XSAVE esteso di NetBSD finché DragonFly non lo porta |
| **B. ABI NetBSD** (kernel v2, libnvmm v2, `stop`) | è l'ABI che QEMU upstream conosce meglio; `stop` senza race | serve `mmap` della comm page o una ABI mista; il codice di base (DragonFly) va adattato in più punti; ogni aggiornamento da DragonFly diventa un merge |
| **C. ABI DragonFly + `stop` come estensione** | prende il meglio | un terzo dialetto NVMM che nessun altro usa: da evitare finché non lo accetta upstream |

### D2. Memoria guest: paginabile o cablata

| Opzione | Pro | Contro |
|---|---|---|
| **A. Paginabile** (`OBJT_SWAP`, fault su EPT violation) — *consigliata come default* | come bhyve e come NVMM originale; la RAM non usata dal guest non è occupata; overcommit possibile | più percorsi da testare (fault, A/D emulati su Ivy Bridge); un guest può finire in swap |
| **B. Cablata** (`vm_map_wire` alla `gpa_map`) | prestazioni prevedibili, meno fault | la RAM della VM è bloccata subito; con ZFS ARC sullo stesso NAS la pressione di memoria è un problema reale |
| **A + opzione B** in seguito (come `bhyve -S`) | flessibile | un flag in più nell'ABI o in un sysctl |

### D3. Cosa fare se vmm.ko viene caricato dopo nvmm.ko

| Opzione | Pro | Contro |
|---|---|---|
| **A. nvmm.ko dichiara anche `MODULE_VERSION(vmm, 1)`** | il linker rifiuta vmm.ko *prima* di eseguire qualunque codice ("interface vmm.1 already present in the KLD 'nvmm.ko'"), quindi niente panic; bhyve fallisce in modo pulito | trucco: `kldstat -v` mostra "vmm" dentro nvmm.ko; un modulo con `MODULE_DEPEND(…, vmm, …)` si legherebbe a noi (in base non ce ne sono) |
| **B. Solo documentazione + controllo al caricamento** | nessun trucco | resta il panic se qualcuno fa `kldload vmm` (o avvia bhyve, che carica vmm da solo) con nvmm attivo |
| **C. Controllo periodico (`CR4`/stato VMX) e blocco delle VM** | rileva il problema | non previene il panic allo scarico di vmm.ko |

Consiglio **A**, con un commento chiaro nel codice e una riga nel README.

### Altre scelte minori (proposta, salvo tuo parere)

- **Nome e posizione del repo:** `~/git-projects/freedomnas/freebsd-nvmm`,
  commit solo locali, nessun remoto finché non lo crei tu.
- **Permessi di `/dev/nvmm`:** `root:wheel 0600` nella fase 1; il gruppo
  dedicato (DragonFly usa `nvmm`) lo decidiamo con il modulo VM.
- **Commit:** messaggi brevi all'imperativo, senza trailer, come negli
  altri repo FreedomNAS.

---

## 7. Build: cosa posso fare io e cosa devi fare tu

La mia macchina è Gentoo su AMD Ryzen: niente FreeBSD nativo e niente VMX
annidato. Posso però **compilare** il modulo dentro una VM FreeBSD
15.1-RELEASE (l'immagine qcow2 ufficiale è già nella cache della CI,
`/var/cache/freedomnas-ci/freebsd/`; userei un overlay in `/dev/shm`,
senza toccare l'originale). Lì faccio solo `make`, nessun `kldload`.

Sulla tua macchina di test, a fase 1 finita, i comandi saranno questi (li
riconfermo con i percorsi esatti quando il codice c'è):

```sh
# sorgenti del kernel 15.1 (se /usr/src/sys non c'è)
git clone --depth 1 -b releng/15.1 https://github.com/freebsd/freebsd-src.git /usr/src
cd /percorso/freebsd-nvmm/sys/modules/nvmm
make SYSDIR=/usr/src/sys
kldload ./nvmm.ko && dmesg | tail
```
