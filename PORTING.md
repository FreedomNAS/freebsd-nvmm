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

### Scelta della base: codice NetBSD + strato di astrazione DragonFly

Con l'ABI NetBSD (decisione **D1**, §6) la base è il codice **NetBSD**:
è lui che implementa quell'ABI (kernel v2, `comm->stop`,
`NVMM_VCPU_CONF_XCR0_MASK`, XSAVE esteso, suspend, correzioni 2026). Da
DragonFly prendiamo il **metodo**, non il frontend:

- `nvmm_os.h` di DragonFly con un ramo `__FreeBSD__` in più (i rami
  NetBSD e DragonFly restano, così i diff con gli originali sono leggibili);
- `nvmm.c` di NetBSD con le chiamate UVM/kmem/rwlock dirette sostituite
  dalle `os_*`, come ha fatto DragonFly; la glue di sistema (autoconf,
  fileops, pmf) esce da `nvmm.c` e va in `nvmm_freebsd.c`;
- le strutture FPU autonome di DragonFly (`struct nvmm_x64_state_fpu`) al
  posto di `struct fxsave` di `<x86/cpu_extended_state.h>`, che FreeBSD
  non ha: stessi nomi di campo e stesso layout (512 byte), quindi l'ABI
  verso QEMU resta quella NetBSD;
- `__BIT`/`__BITS` definiti per FreeBSD dove servono (anche negli header
  visti da userland).

La comm page segue l'ABI NetBSD: l'utente fa `mmap()` su `/dev/nvmm`
all'offset `NVMM_COMM_OFF(machid, cpuid)`. Su FreeBSD questo è
`d_mmap_single` nella `cdevsw`, che restituisce l'oggetto VM della comm
page della macchina con l'offset giusto (lo stesso meccanismo del
`devmem` di bhyve).

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
| sospensione/ripresa S3 (VMXON perso) | pmf | — | `EVENTHANDLER(power_suspend)` → VMXOFF su tutte le CPU, `EVENTHANDLER(power_resume)` → VMXON. Non `vmm_resume_p`: `mp_x86.c` lo chiama solo sulle AP, mai sulla CPU 0 (vedi **F2-3**). |

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
| **1. Scheletro** | `sys/modules/nvmm/Makefile`, `sys/dev/nvmm/`: frontend NetBSD convertito alle `os_*`, ramo `__FreeBSD__` in `nvmm_os.h`, `nvmm_freebsd.c` con device e modulo; `vmx_ident` reale (VMX, EPT, unrestricted guest, INVEPT/INVVPID) senza VMXON; rifiuto se vmm.ko è presente; `/dev/nvmm` con `NVMM_IOC_CAPABILITY` funzionante e `ENOSYS` per il resto | build pulita; `kldload` su i5 → messaggio "attached"; su CPU senza VMX/EPT/UG → rifiuto con messaggio, niente panic; con vmm.ko caricato → rifiuto; un piccolo programma legge la capability; `kldunload` pulito |
| **2. VMXON per CPU** | `vmx_init/fini`: `IA32_FEATURE_CONTROL`, `CR4.VMXE`, regioni VMXON, `smp_rendezvous`, VMXOFF allo scarico, sospensione/ripresa (F2-1..3) | load/unload ripetuti 100 volte; `kldload vmm` dopo nvmm rifiutato (secondo **D3**) |
| **3. Macchina e vCPU** | vmspace EPT, VMCS, VPID, MSR/I/O bitmap, comm page nel processo, ioctl create/destroy/configure/setstate/getstate | creazione e distruzione di 128 macchine; uscita del processo senza distruggere → nessun leak (`vmstat -m`, `sysctl vm.stats`) |
| **4. Memoria guest** | `hva_map/unmap`, `gpa_map/unmap`, oggetti VM, fault, protocollo `pm_eptgen` | test che mappa, scrive dall'host e rilegge dal pmap guest; unmap mentre un'altra vCPU gira |
| **5. vcpu_run** | ciclo di esecuzione, uscite, iniezione eventi, FPU, segnali | guest minimale (pochi byte di codice in real mode che fa `out` e `hlt`), poi un kernel Linux con `-kernel` |
| **6. libnvmm** | port di `lib/libnvmm` DragonFly (Makefile `bsd.lib.mk`, header), nvmmctl(8) | i test di libnvmm (`tests/lib/libnvmm` di NetBSD/DragonFly) |
| **7. QEMU `accel=nvmm`** | patch a `meson.build` (`host_os == 'freebsd'`), header, port in `emulators/qemu` locale | FreeBSD e Linux guest con `-accel nvmm`, confronto con TCG; poi l'integrazione nel modulo VM di FreedomNAS |
| **8. SVM** | backend AMD | come le fasi 5–7 su una macchina AMD |

Fasi trasversali, da fare quando servono: suspend/resume, incrocio delle
correzioni NetBSD↔DragonFly, sysctl/statistiche, pacchetto FreeBSD.

---

## 6. Decisioni prese (2026-11-24)

| | Decisione | Note |
|---|---|---|
| **D1. ABI verso userland** | **NetBSD**: kernel v2, libnvmm v2, `comm->stop` / `nvmm_vcpu_stop()`, `NVMM_VCPU_CONF_XCR0_MASK` | più completa; QEMU la usa già com'è. Più lavoro: base NetBSD convertita alle `os_*` (vedi §2) e comm page via `d_mmap_single`. |
| **D2. Memoria guest** | **paginabile** (`OBJT_SWAP`, fault su EPT violation) | la cablata si aggiunge dopo, quando la paginabile è stabile: una cosa alla volta, per sapere da dove arrivano i problemi. |
| **D3. vmm.ko caricato dopo nvmm.ko** | nvmm.ko dichiara anche **`MODULE_VERSION(vmm, 1)`** | il linker rifiuta vmm.ko prima di eseguirne il codice; più il controllo al caricamento di nvmm.ko se vmm è già presente. |
| Permessi di `/dev/nvmm` | `root:wheel 0600` nella fase 1 | il gruppo dedicato si decide con il modulo VM di FreedomNAS. |
| Repo | `~/git-projects/freedomnas/freebsd-nvmm`, commit locali | nessun remoto finché non lo crei tu. |
| Copyright dei file nuovi | `Sarah Chiandotto` | confermato 2026-11-25. |

### Decisioni della fase 2 (2026-11-25)

| | Decisione | Note |
|---|---|---|
| **F2-1. VMXON fallisce su una CPU** | **rifiuto pulito**: esito registrato CPU per CPU, VMXOFF su quelle già accese, `kldload` fallisce con messaggio che nomina la CPU | NetBSD fa panic; senza console sarebbe una macchina che smette di rispondere. |
| **F2-2. `CR4.VMXE` già acceso al caricamento** | **rifiuto**: "VMX already in use by another hypervisor" | un altro hypervisor (es. VirtualBox con una VM accesa) è in VMX root; allo scarico gli spegneremmo VMX sotto i piedi. |
| **F2-3. Ripresa da S3** | **subito**, via `power_suspend`/`power_resume` → `nvmm_suspend()`/`nvmm_resume()` del frontend → VMXOFF/VMXON su tutte le CPU | scelto `power_*` invece di `vmm_resume_p` perché quest'ultimo non copre la CPU 0. Non provabile sulla macchina senza schermo. |

### Decisioni della fase 3 (2026-11-25)

| | Decisione | Note |
|---|---|---|
| **P3-1. VMCS e migrazione delle vCPU** | **come NetBSD/DragonFly**: la VMCS resta caricata sull'ultima CPU, VMCLEAR remoto via IPI (`smp_rendezvous_cpus`) solo quando la vCPU cambia CPU | codice già provato in due sistemi, portato quasi riga per riga. |
| **P3-2. Verifica dei leak** | **contatori sysctl in sola lettura** `hw.nvmm.machines`, `hw.nvmm.vcpus`, `hw.nvmm.kpages` | devono tornare a 0 dopo ogni prova; utili poi al modulo VM di FreedomNAS. |
| **P3-3. Strumento di prova** | **`tools/nvmmtest`** con ioctl diretti ora; **i test di libnvmm di NetBSD dopo la fase 5** | una cosa alla volta: libnvmm resta alla fase 6, i suoi test servono `vcpu_run`. |

Per il resto la fase 3 segue bhyve e NetBSD: vmspace con pmap EPT
(`pmap_pinit_type(PT_EPT)`, `PMAP_EMULATE_AD_BITS` senza bit A/D), vettore
IPI per le invalidazioni EPT allocato al caricamento con `lapic_ipi_alloc`
(ripiego `IPI_AST`, anticipato dalla fase 5 perché il pmap EPT lo vuole già
alla creazione), ioctl aperti solo per macchine e vCPU (memoria e
`vcpu_run` restano `ENOSYS`).

### Decisioni della fase 4 (2026-11-25)

| | Decisione | Note |
|---|---|---|
| **P4-1. Provare la memoria prima che un guest giri** | **ioctl di debug solo nella build di prova** (`make -DNVMM_DEBUG`): dato un gpa, fault attraverso il pmap EPT e lettura/scrittura della pagina fisica che il guest vedrebbe | la parte più rischiosa (fault e mappature EPT) provata da sola, prima di mescolarla con `vcpu_run`. Il modulo normale non ha l'ioctl. |

Fault come bhyve: `pmap_emulate_accessed_dirty()` (solo CPU senza bit A/D
dell'EPT, es. Ivy Bridge) e poi `vm_fault()` sul vmspace EPT. Sulla
macchina di test (Haswell, bit A/D presenti) l'emulazione non viene
esercitata. Il protocollo di invalidazione (`pm_active`, `pm_eptgen`, SMR)
serve solo con un guest in esecuzione: fase 5.

### Decisioni della fase 5 (2026-11-25)

| | Decisione | Note |
|---|---|---|
| **P5-1. Sotto-passi** | **5a** assembly, ciclo, uscite HLT/I/O/EPT violation, interrupt e NMI; **5b** uscite MMIO e protocollo EPT sotto carico; **5c** CPUID con maschere, MSR, CR, XSETBV; **5d** FPU, registri di debug, TSC; **5e** segnali, `comm->stop`, prelazione. Ognuno provato sulla macchina | senza console un panic va attribuito a un pezzo solo. L'uscita EPT violation è passata dal 5b al 5a: con memoria paginabile ogni primo accesso a una pagina la produce, il guest non legge nemmeno la prima istruzione senza. |
| **P5-2. FPU** | **come NetBSD**: `fpu_kern_enter(FPU_KERN_NOCTX)` e XRSTOR/XSAVE dello stato guest intorno a ogni ingresso | semplice, e lo stato FPU dei processi host non è mai esposto al guest; l'ottimizzazione pigra alla bhyve si valuta dopo. |
| **P5-3. Criterio di fine fase** | **micro-guest in `nvmmtest`**; l'avvio di Linux passa alla fase 7 con QEMU | senza libnvmm e QEMU servirebbe un loader usa e getta. |

Seguono bhyve senza scelte: protocollo EPT (`pm_active`, SMR, `pm_eptgen`,
INVEPT), ripristino di GDTR/IDTR (limiti) e LDTR dopo ogni uscita, NMI
dell'host rilanciati con `int $2`.

Il resto di `vmx_init` NetBSD (bitmap VPID, tipo di INVEPT/INVVPID, tipo
EPTP, L1TF, leaf CPUID massimi) arriva con la fase 3, dove viene usato. Il
vettore IPI per i kick delle vCPU arriva con la fase 5.

---

## 7. Build e test

La compilazione si fa sulla macchina di test (Intel i5), avviata da un
disco dedicato con **solo FreeBSD 15.1**: nessun FreedomNAS, così il
disco di FreedomNAS resta com'è.

Il disco (Hitachi 320 GB) si prepara dal PC con
`tools/devbox/make-devbox.sh`. Lo script usa `freebsd/qemu-driver.sh`
dell'image builder senza modificarlo: avvia l'immagine VM ufficiale di
FreeBSD 15.1 e le passa il disco come secondo disco, poi
`tools/devbox/devbox-in-guest.sh` fa questo:

- GPT su tutto il disco: `freebsd-boot` (gptboot, avvio BIOS), ESP FAT32
  (avvio UEFI), swap 8 GiB, UFS per il resto; etichette `nvmm*`, quindi il
  disco si sposta su qualunque macchina;
- sistema base da pkgbase (`FreeBSD-set-minimal`, `-base`, `-kernels`:
  compilatore e header inclusi) all'ultimo patch level di 15.1, con il
  repository FreeBSD-base attivo per `pkg upgrade`;
- `git-lite`, `sudo`, `rsync`; sorgenti `releng/15.1` in `/usr/src`
  (stesso patch level del kernel);
- `sshd`, DHCP su qualunque scheda, hostname `nvmm-dev`, utente `admin`
  (gruppo wheel, sudo senza password) e `root` con password
  `freedomnas` e la tua chiave SSH.

## 8. Stato della fase 1 (2026-11-24)

Fatto, compilato pulito (`-Werror`) sul disco di sviluppo (15.1-p7):

- `nvmm.ko`: frontend NetBSD sulle `os_*`, glue FreeBSD (`/dev/nvmm`
  `root:wheel 0600`, `d_mmap_single` per la comm page), `vmx_ident`
  completo con letture MSR protette (`rdmsr_safe`) e messaggi per VMX
  disabilitato nel firmware, EPT, unrestricted guest e VPID mancanti;
  `NVMM_IOC_CAPABILITY` funzionante, gli altri ioctl rispondono `ENOSYS`;
- tutti i simboli esterni del modulo esistono nel kernel 15.1-p7;
- `tools/nvmmcap`: legge la capability e controlla `ENOSYS`.

Provato in una VM FreeBSD usa e getta su CPU AMD (mai sul PC di lavoro):
rifiuto pulito "this CPU is not supported"; con vmm.ko caricato il linker
rifiuta nvmm.ko. La prima versione andava in **panic** proprio nel rifiuto:
FreeBSD manda `MOD_UNLOAD` anche dopo un `MOD_LOAD` fallito, e
`nvmm_fini()` girava su un modulo mai inizializzato. Corretto.

Correzioni a bug NetBSD trovati nel porting (da segnalare upstream):
`nvmm_enter_sig()` restituisce 0 dopo un'attesa interrotta (poi
`nvmm_exit()` sbilancia il contatore), `nvmm_kill_machines()` perde la comm
object, `gpa_unmap` rifiuta l'ultima pagina (DragonFly l'ha già corretto).

### Test sulla macchina di test (i5)

```sh
cd ~admin && git clone <questo repo> freebsd-nvmm   # o rsync dal PC
cd freebsd-nvmm/sys/modules/nvmm && make
cd ../../../tools/nvmmcap && make

sudo kldload ../../sys/modules/nvmm/nvmm.ko && dmesg | tail -3
#   atteso: "nvmm: attached, using backend x86-vmx"
./nvmmcap                     # atteso: version 2 ... "OK"
sudo kldload vmm              # atteso: rifiutato,
dmesg | tail -2               #   "interface vmm.1 already present in the KLD 'nvmm.ko'"
sudo kldunload nvmm           # atteso: pulito
sudo kldload vmm && sudo kldload ../../sys/modules/nvmm/nvmm.ko
#   atteso: nvmm rifiutato dal linker; poi: sudo kldunload vmm
```

Con `boot -v` (o `sysctl debug.bootverbose=1` prima del load) il modulo
stampa anche se l'EPT ha i bit A/D (Haswell sì, Ivy Bridge no) e la
maschera XCR0.

Sulla macchina di test:

```sh
cd /percorso/freebsd-nvmm/sys/modules/nvmm
make
kldload ./nvmm.ko && dmesg | tail      # solo tu, sull'hardware di test
```

Io lì compilo e basta: `kldload`/`kldunload` li fai tu.

### Risultato sulla macchina di test (2026-11-25)

Intel Core i5-4440 (Haswell), FreeBSD 15.1-RELEASE-p7, disco
`netfreebsd` via USB. Repo copiato con `git archive` + `tar` su SSH,
build pulita (`-Werror`) di `nvmm.ko` e `nvmmcap` sulla macchina.

| Prova | Risultato |
|---|---|
| `kldload nvmm.ko` (bootverbose) | ok: "VMX EPT A/D bits present, XCR0 mask 0x7", "attached, using backend x86-vmx"; `/dev/nvmm` `root:wheel 0600` |
| `nvmmcap` | ok: version 2, state_size 1008, 128 macchine, 256 vCPU, xcr0 0x7, `MACHINE_CREATE` → `ENOSYS` |
| `kldload vmm` con nvmm caricato | rifiutato da `kldload(8)` stesso ("module already loaded or in kernel"): trova `vmm` via `modfind` grazie a `MODULE_VERSION(vmm, 1)`, prima ancora del linker |
| `kldunload nvmm` | ok, `/dev/nvmm` sparito |
| `kldload vmm` poi `nvmm.ko` | nvmm rifiutato dal linker: "interface vmm.1 already present in the KLD 'vmm.ko'!" |
| `kldunload vmm` | ok, sistema pulito |

Nessun panic. Il messaggio "failed to load CTF for nvmm.ko" è solo
informativo (modulo senza sezione CTF per DTrace). Fase 1 chiusa.

## 9. Fase 2: VMXON per CPU (2026-11-25)

Fatto (commit `8f542fc`, `70349a8`): regioni VMXON per CPU, lock di
`IA32_FEATURE_CONTROL` se libero, `CR4.VMXE` + VMXON in `smp_rendezvous`
con esito per CPU; rifiuto pulito se `CR4.VMXE` è già acceso (F2-2) o se
VMXON fallisce su una CPU, con rollback sulle altre (F2-1); VMXOFF allo
scarico; sospensione/ripresa via `power_suspend`/`power_resume` (F2-3).

Sulla macchina di test (i5-4440, 4 CPU):

| Prova | Risultato |
|---|---|
| `kldload` (bootverbose) | ok: "VMXON on 4 CPUs", "attached, using backend x86-vmx" |
| `kldload vmm` con nvmm caricato | rifiutato |
| 100 cicli `kldload`/`kldunload` | 100/100, nessun messaggio d'errore. Ogni caricamento ricontrolla `CR4.VMXE` su tutte le CPU, quindi ogni giro verifica anche che lo scarico precedente abbia fatto VMXOFF ovunque |
| a fine prova | nessun modulo caricato |

Non provato: sospensione S3 (macchina senza schermo) e i rami d'errore
(VMXE occupato, VMXON fallito), che su questo hardware non si presentano.

## 10. Fase 3: macchine e vCPU (2026-11-25)

Fatto (commit `0c603ed`…`d07c370`): vmspace con pmap EPT come bhyve,
vettore IPI allocato al caricamento, contatori `hw.nvmm.*`,
`nvmm_x86.c` su FreeBSD, backend VMX per macchine e vCPU (VMCS con
VMCLEAR remoto, MSR bitmap e lista MSR guest, VPID con mutex spin perché
preso nella sezione critica della VMCS, setstate/getstate, configure
CPUID/TPR/XCR0, iniezione eventi, suspend/resume per vCPU), ioctl aperti
tranne memoria e `vcpu_run`, `tools/nvmmtest`.

Bug NetBSD/DragonFly trovati in questa fase (da segnalare):
- stato di RESET con `fx_mxcsr_mask` a 0: setstate fa `mxcsr &= mask` e
  una vCPU nuova parte con MXCSR 0 invece di 0x1F80 (QEMU lo nasconde
  passando sempre la sua maschera);
- `vmx_vcpu_create`/`vmx_vcpu_destroy` liberano la cpudata con
  `sizeof(*cpudata)`, mentre è allocata con l'area XSAVE in coda.

Sulla macchina di test (i5-4440), `nvmmtest` 3 giri di fila, tutti OK:

| Prova | Risultato |
|---|---|
| 128 macchine, la 129ª `ENOBUFS`, distruzione doppia rifiutata | ok |
| 256 vCPU in una macchina, oltre il limite e doppione rifiutati | ok |
| stato di RESET (RIP, CS, CR0, CR4 senza VMXE, DR7, FPU, MXCSR) | ok |
| setstate/getstate a 64 bit (GPR, segmenti, CR, DR, MSR, FPU) | ok |
| configure CPUID/TPR/XCR0 validi e non validi, iniezione #GP | ok |
| `VCPU_RUN`, `GPA_MAP`, `HVA_MAP` → `ENOSYS` | ok |
| figlio con 4 macchine × 4 vCPU che esce senza distruggere | ok: nel figlio `kpages 68` (4 VMXON + 16 vCPU × 4 pagine), dopo l'uscita di nuovo 4 |
| contatori `hw.nvmm.*` dopo ogni passo, `kldunload` finale | tornati ai valori iniziali, scarico pulito |

## 11. Fase 4: memoria guest (2026-11-25)

Fatto (commit `11caaa9`, `6405318`, `86e59a9`): fault come bhyve
(`pmap_emulate_accessed_dirty()` e poi `vm_fault()` sul vmspace EPT),
ioctl `hva_map/unmap` e `gpa_map/unmap` aperti (resta `ENOSYS` solo
`vcpu_run`), ioctl di prova `NVMM_IOC_DEBUG_GPA` solo con
`make -DNVMM_DEBUG` (`hw.nvmm.debug` = 1), prove di memoria in
`nvmmtest`.

Sulla macchina di test (i5-4440): build di debug 3 giri, build normale 1
giro, tutti OK, scarico pulito.

| Prova | Risultato |
|---|---|
| errori di `hva_map` (sovrapposizione, non allineato) e `gpa_map` (hva non mappato, oltre `max_ram`, non allineato, prot non valida) | ok |
| host scrive → guest legge, anche dall'alias in sola lettura | ok |
| guest scrive → host legge | ok |
| pagina mai toccata letta dal guest: tutta a zero | ok |
| scrittura sull'alias in sola lettura e lettura di gpa non mappato → `EFAULT` | ok |
| dopo `gpa_unmap` la lettura fallisce, l'alias resta valido | ok |
| `hva_unmap` doppio → `ENOENT` | ok |
| macchina distrutta con la memoria mappata: l'host la usa fino al `munmap` | ok |
| figlio che esce con 4 macchine, 16 vCPU e memoria scritta | ok (`vmobjs 8` nel figlio, 0 dopo) |
| contatori a fine giro, build normale senza l'ioctl di prova | ok |

Non provato: emulazione dei bit A/D (Haswell li ha), invalidazioni EPT con
una vCPU in esecuzione (fase 5).

## 12. Fase 5 (in corso)

### 5a: assembly, ciclo, HLT, I/O, EPT violation, interrupt, NMI (2026-11-25)

Commit `1edfa41`, `3f66521`, `6594a84`, `8ff5764`. Primo giro: nessun
panic, ma il guest non partiva (uscita 48, EPT violation, che avevo messo
nel 5b: con memoria paginabile serve dalla prima istruzione). Corretto.

Sulla macchina di test, micro-guest in real mode (`nvmmtest run`) e poi
`nvmmtest` completo, OK:

| Prova | Risultato |
|---|---|
| `out 0x80,ax` due volte, valori 0x1234 e 0x1235, `npc` giusto | ok |
| `sti; hlt` → uscita HALTED | ok |
| interrupt 0x20 iniettato: handler `out 0x81,al` (0x55), `iret`, di nuovo HALTED | ok |
| 100000 uscite HLT fino a userland | ok, 3,35 µs l'una (ioctl, entry/exit, FPU) |
| vCPU spostata su tutte e 4 le CPU (VMCLEAR remoto, campi host per CPU) | ok |
| contatori e `kldunload` a fine prova | ok |

### 5b: MMIO e protocollo EPT con la vCPU in esecuzione (2026-11-25)

Nessun cambio nel kernel; prova `b90a290`. OK sulla macchina di test:

| Prova | Risultato |
|---|---|
| scrittura a gpa non mappato 0x5000 → uscita MEMORY, `prot` WRITE, RIP sull'istruzione | ok |
| guest che legge in un ciclo stretto gpa 0x3000 mentre un altro thread la rimappa 1000 volte su pagine con contenuti diversi | ok: ogni valore nuovo visto dal guest (circa 1 ms a giro, è il passo di attesa del test), nessuna traduzione EPT rimasta vecchia |

### 5c: CPUID, MSR, CR, XSETBV, MONITOR/MWAIT (2026-11-25)

Commit `68921a2`, `28434a5`, `f0fb381`. OK sulla macchina di test:

| Prova | Risultato |
|---|---|
| CPUID 0: vendor dell'host, foglia massima limitata | ok (`GenuineIntel`, 0xD) |
| CPUID 0x40000000: firma `___ NVMM ___` | ok |
| CPUID 1: VMX nascosto, bit hypervisor acceso, APIC ID = vCPU | ok |
| CPUID 6 configurata con `exit`: uscita CPUID, risposta di userland | ok |
| RDMSR 0x1B (APIC base): uscita RDMSR, risposta di userland | ok |
| RDMSR PAT nel kernel: valore di reset del guest | ok |
| scrittura di CR0 (CD/NW tolti) gestita nel kernel | ok |
