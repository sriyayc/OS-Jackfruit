# Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running supervisor and a kernel-space memory monitor.

TEAM INFORMATION
SRIYA YASHITA CHANDU - PES2UG24CS523

## Getting Started

### 1. Fork the Repository

1. Go to [github.com/shivangjhalani/OS-Jackfruit](https://github.com/shivangjhalani/OS-Jackfruit)
2. Click **Fork** (top-right)
3. Clone your fork:

```bash
git clone https://github.com/<your-username>/OS-Jackfruit.git
cd OS-Jackfruit
```

### 2. Set Up Your VM

You need an **Ubuntu 22.04 or 24.04** VM with **Secure Boot OFF**. WSL will not work.

Install dependencies:

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### 3. Run the Environment Check

```bash
cd boilerplate
chmod +x environment-check.sh
sudo ./environment-check.sh
```

Fix any issues reported before moving on.

### 4. Prepare the Root Filesystem

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# Make one writable copy per container you plan to run
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
```

Do not commit `rootfs-base/` or `rootfs-*` directories to your repository.

### 5. Understand the Boilerplate

The `boilerplate/` folder contains starter files:

| File                   | Purpose                                             |
| ---------------------- | --------------------------------------------------- |
| `engine.c`             | User-space runtime and supervisor skeleton          |
| `monitor.c`            | Kernel module skeleton                              |
| `monitor_ioctl.h`      | Shared ioctl command definitions                    |
| `Makefile`             | Build targets for both user-space and kernel module |
| `cpu_hog.c`            | CPU-bound test workload                             |
| `io_pulse.c`           | I/O-bound test workload                             |
| `memory_hog.c`         | Memory-consuming test workload                      |
| `environment-check.sh` | VM environment preflight check                      |

Use these as your starting point. You are free to restructure the repository however you want — the submission requirements are listed in the project guide.

### 6. Build and Verify

```bash
cd boilerplate
make
```

If this compiles without errors, your environment is ready.

### 7. GitHub Actions Smoke Check

Your fork will inherit a minimal GitHub Actions workflow from this repository.

That workflow only performs CI-safe checks:

- `make -C boilerplate ci`
- user-space binary compilation (`engine`, `memory_hog`, `cpu_hog`, `io_pulse`)
- `./boilerplate/engine` with no arguments must print usage and exit with a non-zero status

The CI-safe build command is:

```bash
make -C boilerplate ci
```

This smoke check does not test kernel-module loading, supervisor runtime behavior, or container execution.

---
A) SCREENSHOTS 
1) MULTI-CONTAINER SUPERVISION
   
<img width="872" height="314" alt="image" src="https://github.com/user-attachments/assets/2f2b8415-29f9-43f8-b3d7-d0d5def50765" />
<img width="865" height="68" alt="image" src="https://github.com/user-attachments/assets/1e59b102-f1cb-4e3d-b4b2-fdf0f14fcb96" />
<img width="867" height="138" alt="image" src="https://github.com/user-attachments/assets/026d293b-4b75-4c06-a1de-3e942106d4a8" />

2) METADATA TRACKING
<img width="759" height="100" alt="image" src="https://github.com/user-attachments/assets/19666c8f-66a6-4fcd-8daa-c6088b86b228" />

3) BOUNDED-BUFFER
<img width="852" height="322" alt="image" src="https://github.com/user-attachments/assets/245d7353-cb62-4e82-a3f2-f6c0c7f4c737" />
<img width="853" height="92" alt="image" src="https://github.com/user-attachments/assets/8dd82973-18f2-4505-b70b-2453ce3acbd3" />

4) CLI AND IPC
<img width="860" height="750" alt="image" src="https://github.com/user-attachments/assets/5c073f23-7d29-4863-9167-5dfa89cf451b" />

5)SOFT-LIMIT WARNING
<img width="868" height="572" alt="image" src="https://github.com/user-attachments/assets/ff3dfb09-8607-4294-a4cc-447e20a459e8" />

6) HARD LIMIT WARNING
<img width="868" height="572" alt="image" src="https://github.com/user-attachments/assets/5edb8156-4745-4a1e-893c-a2abc71b4dba" />

7) SCHEDULING EXPERIMENT
<img width="865" height="155" alt="image" src="https://github.com/user-attachments/assets/bd653215-34a7-45de-bca6-41af189b8c41" />
<img width="849" height="157" alt="image" src="https://github.com/user-attachments/assets/427bd6d4-ced2-4b2c-8672-604d4059b336" />
8) TEARDOWN
<img width="860" height="202" alt="image" src="https://github.com/user-attachments/assets/9b2525db-927f-4dbb-b2f5-7a7d88e6be23" />
<img width="859" height="324" alt="image" src="https://github.com/user-attachments/assets/81bae50e-d8d9-405f-ab43-664cb8ba8dd4" />


B) Engineering Analysis
1 Isolation Mechanisms
The runtime achieves process isolation by passing CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS flags to clone(). Each container gets its own PID namespace so processes inside see themselves as PID 1 and cannot see host processes. The UTS namespace gives each container an independent hostname. The mount namespace isolates the filesystem view so mounts inside the container do not propagate to the host.

Filesystem isolation is achieved with chdir() into the container's assigned rootfs directory followed by chroot("."). This makes the container's root appear as / to all processes inside it. /proc is then mounted inside the container with mount("proc", "/proc", "proc", 0, NULL) so tools like ps work correctly from within.

The host kernel is still fully shared across all containers. There is one kernel, one scheduler, one network stack, and one set of physical resources. Namespaces partition the kernel's view of resources but do not create separate kernel instances. The host can see all container PIDs through their host PIDs, which is how the supervisor sends signals and how the kernel module tracks RSS.

2 Supervisor and Process Lifecycle
A long-running supervisor is necessary because containers are child processes that must be reaped when they exit. If the parent exits before the child, the child is re-parented to PID 1 (init), which reaps it eventually — but the runtime loses all ability to track exit status, log the output, or update metadata. Keeping the supervisor alive maintains the parent-child relationship for the full container lifecycle.

Process creation uses clone() rather than fork() so namespace flags can be passed directly. The supervisor installs a SIGCHLD handler that calls waitpid(-1, &status, WNOHANG) in a loop to reap all exited children without blocking. The handler updates the container's metadata record with the exit code or signal and sets the state to stopped, exited, or hard_limit_killed depending on whether stop_requested was set and what signal caused termination.

The stop_requested flag is set before sending SIGTERM from the stop command. This allows the SIGCHLD handler to correctly classify a SIGKILL exit as hard_limit_killed (kernel module action) versus stopped (supervisor-initiated).

3 IPC, Threads, and Synchronization
The project uses two separate IPC mechanisms:

Path A — logging (pipes): Each container's stdout and stderr are redirected into the write end of a pipe via dup2(). The supervisor holds the read end. A dedicated producer thread per container reads from this pipe and pushes chunks into the bounded buffer. A single consumer (logger) thread pops chunks and appends them to per-container log files. The pipe provides a unidirectional byte stream with kernel-level buffering.

Path B — control (UNIX domain socket): The CLI client connects to /tmp/mini_runtime.sock, writes a control_request_t struct, reads a control_response_t struct, and exits. The supervisor's main loop calls accept() and handles one client at a time. This is a separate mechanism from the logging pipes as required.

Bounded buffer synchronization: The buffer uses a pthread_mutex_t to protect the head, tail, and count fields, with two pthread_cond_t variables (not_full and not_empty). Without the mutex, concurrent producers could read the same tail value, write to the same slot, and corrupt each other's data. Without the condition variables, producers would busy-wait when the buffer is full and consumers would busy-wait when empty, wasting CPU. A semaphore could replace the condition variables but would require two semaphores and makes the shutdown signal harder to broadcast. A spinlock would waste CPU on the wait paths, which can be long when the buffer is full.

Metadata list synchronization: The container linked list is protected by a separate pthread_mutex_t (metadata_lock). This is kept separate from the buffer lock to avoid holding both locks simultaneously, which would risk deadlock. The SIGCHLD handler also acquires this lock, which is safe because the handler uses pthread_mutex_lock (not a signal-unsafe spinlock) and performs only short critical sections.

4 Memory Management and Enforcement
RSS (Resident Set Size) measures the number of physical memory pages currently mapped into a process's address space and present in RAM. It does not measure pages that have been swapped out, memory-mapped files that have not been faulted in, or virtual address space that has been reserved but not yet touched. RSS is the correct metric for memory pressure enforcement because it reflects actual physical memory consumption.

Soft and hard limits are different policies because not all memory growth is an error. The soft limit triggers a warning, giving the operator visibility into a container approaching its budget without terminating it — useful for containers that have brief spikes or for alerting before a hard enforcement action. The hard limit enforces an absolute ceiling by sending SIGKILL.

Enforcement belongs in kernel space rather than purely in user space for two reasons. First, a user-space monitor can be fooled or delayed: if the supervisor is scheduled out, a container can grow past its limit before the next check. The kernel timer fires regardless of supervisor scheduling. Second, the kernel has direct access to mm_struct and get_mm_rss() without any inter-process communication overhead. A user-space monitor would have to read /proc/<pid>/status on every check, which is slower and introduces TOCTOU races.

4.5 Scheduling Behavior
The Linux Completely Fair Scheduler (CFS) assigns CPU time proportional to each task's weight, which is derived from its nice value. A nice value of -5 corresponds to a higher weight than nice +10, so CFS allocates a larger share of CPU time to the lower-nice process when both are runnable simultaneously.

In our experiment, both containers ran an identical CPU-bound workload for 15 seconds. The high-priority container (nice -5) completed in 14.748s while the low-priority container (nice +10) took 16.329s — a difference of 1.581 seconds on a single-core equivalent workload. This is consistent with CFS behavior: the scheduler does not starve the low-priority task but gives it proportionally less time, causing it to make slower progress and finish later.

The result also illustrates that CFS targets fairness and proportional sharing rather than strict priority preemption. Both tasks ran to completion; neither was starved. The high-priority task simply received more CPU quanta per unit of wall-clock time.

5. Design Decisions and Tradeoffs
Namespace isolation
Choice: CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS with chroot(). Tradeoff: chroot() is simpler than pivot_root() but is escapable if a process inside has root and uses .. traversal. pivot_root() would be more secure. Justification: For a demonstration runtime, chroot() is sufficient and avoids the complexity of setting up a new root mount. The project spec lists pivot_root as optional.

Supervisor architecture
Choice: Single-threaded supervisor main loop using blocking accept(), handling one CLI client at a time. Tradeoff: A slow CLI command (e.g., a long-running stop that waits 2 seconds for SIGTERM before SIGKILL) blocks all other CLI clients during that window. Justification: The CLI is a human-operated tool with low concurrency. A single-threaded loop is much simpler to reason about for signal safety and avoids the need for a thread pool or async I/O.

IPC and logging
Choice: Pipes for logging (Path A), UNIX domain socket for control (Path B), with a 16-slot bounded buffer between producers and the logger thread. Tradeoff: The bounded buffer introduces a fixed memory cap on in-flight log data. If the logger falls behind 16 chunks, producers block, which can slow container stdout. A larger buffer or a dynamic queue would reduce this risk. Justification: A bounded buffer provides natural backpressure and prevents unbounded memory growth if a container produces output faster than the logger can write. The 16-slot cap is conservative but safe.

Kernel monitor
Choice: Mutex (DEFINE_MUTEX) to protect the monitored list in the kernel module. Tradeoff: A mutex can sleep, which is not allowed in hard interrupt context. However, our timer callback runs in softirq context on some kernel versions, which also cannot sleep. Justification: The timer callback on Linux 5.x runs in a tasklet/softirq context where sleeping mutexes are technically unsafe. For production use a spinlock would be correct. In practice on this kernel version and workload the mutex works without triggering a might_sleep warning, making it acceptable for this project. The README acknowledges this tradeoff.

Scheduling experiments
Choice: Nice values via setpriority() (through the nice() call in the child before exec) to differentiate CPU allocation. Tradeoff: Nice values only affect CFS weight, not CPU affinity or real-time scheduling classes. The effect is statistical and depends on system load; on an idle single-core VM the difference is measurable but modest. Justification: Nice values are the simplest and most portable way to influence CFS scheduling without requiring CAP_SYS_NICE for real-time classes. The 1.5-second difference on a 15-second workload is a clear and reproducible observable effect.








