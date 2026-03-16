# Fleet Scheduling Simulator
**C · System V IPC · POSIX · Linux**

---

## What is this?

A multi-process delivery fleet simulator written in C. You have a grid, some trucks, and a stream of packages that keep arriving each turn — each with a pickup spot, a dropoff spot, and a deadline. Your job is to coordinate the trucks to deliver everything before it expires.

The interesting part isn't the routing — it's how the processes talk to each other. The scheduler (your code) and the helper (the environment) are completely separate OS processes. They share no function calls, no global variables, nothing. The only communication is through the kernel — shared memory for state, message queues for synchronization. That's the whole point.

---

## How it works

Every turn:
1. Helper writes new package arrivals + truck positions into shared memory
2. Helper sends a message: "new turn, here's what changed"
3. Your process reads the state, decides what each truck should do
4. Your process writes commands back into shared memory
5. Your process sends a message: "ready, process my commands"
6. Helper executes the moves, repeat

There's also an auth string requirement — before a truck carrying packages can move, you need to produce the correct sequence of characters for that turn. Gets verified before the move goes through.

---

## OS concepts used

- **System V Shared Memory** — `shmget` / `shmat`. The main state lives here. Both the helper and your process read and write it directly, no copying.
- **System V Message Queues** — `msgget` / `msgsnd` / `msgrcv`. Used for the turn handshake. Your process blocks on `msgrcv` waiting for the helper, helper blocks waiting for you.
- **`fork` + `exec`** — helper, solvers, and student all run as separate processes. The simulator forks them all and coordinates via IPC.
- **Multi-process synchronization** — the turn protocol is a strict request/response. Getting the ordering wrong deadlocks everything.

---

## Scheduling algorithm

Package assignment uses a cost function per truck:
```
cost = distance_to_pickup + 200 / turns_until_expiry
```
Urgent packages get assigned first. Once assigned, trucks route to pickup then dropoff, always prioritizing the earliest-expiring package they're carrying.

---

## Files
```
include/shared.h   — IPC struct definitions (the contract between processes)
src/solution.c     — the scheduler: assignment, routing, auth, turn loop
src/simulator.c    — the test harness: helper + solver processes
Makefile
```

---

## Running it

Needs Linux or GitHub Codespaces.
```bash
make all
./simulator 42        # deterministic run with seed 42
./simulator           # random seed
make ipc-clean        # if it crashes and IPC objects get stuck
```

Output looks like:
```
[ 1] T0=(0,1,p=0) T1=(1,0,p=0) T2=(0,1,p=0) pkgs=3 del=0 exp=0
[ 2] T0=(0,1,p=1) T1=(1,0,p=1) T2=(0,2,p=0) pkgs=4 del=0 exp=0
...
[50] T0=(7,2,p=1) T1=(2,0,p=1) T2=(3,6,p=1) pkgs=123 del=7 exp=5

=== DONE ===
packages=123 delivered=7 expired=5
```

Each column is truck position (x,y) and how many packages it's carrying. `del` is total delivered, `exp` is delivered after deadline.