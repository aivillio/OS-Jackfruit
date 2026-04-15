

## 0. One-time prep

Run these once after a fresh clone or whenever you rebuild the binaries/rootfs from scratch.

### One-time setup command
```bash
cd /boilerplate

# Build user-space and kernel module
make
make module

# Build static workload binaries for Alpine/musl rootfs
sudo apt-get update
sudo apt-get install -y musl-tools
musl-gcc -O2 -static -o memory_hog memory_hog.c
musl-gcc -O2 -static -o cpu_hog cpu_hog.c
musl-gcc -O2 -static -o io_pulse io_pulse.c

# Copy workloads into rootfs copies
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
cp -f memory_hog cpu_hog io_pulse rootfs-alpha/
cp -f memory_hog cpu_hog io_pulse rootfs-beta/
chmod +x rootfs-alpha/memory_hog rootfs-alpha/cpu_hog rootfs-alpha/io_pulse
chmod +x rootfs-beta/memory_hog rootfs-beta/cpu_hog rootfs-beta/io_pulse
```

### Before running the supervisor

If the kernel module is not already loaded for the current boot, load it once and verify the device node:

```bash
cd /boilerplate
sudo rmmod monitor 2>/dev/null || true
sudo insmod monitor.ko
ls -l /dev/container_monitor
```

Before every supervisor run,  need the module loaded and the container rootfs copies present.

If `rootfs-alpha/` and `rootfs-beta/` already exist,  do **not** need to rebuild anything or recopy them.
If the module is already loaded, you can skip `rmmod`/`insmod`.

Minimal pre-supervisor setup:

```bash
cd /boilerplate
ls -l /dev/container_monitor || sudo insmod monitor.ko
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
```

If you changed the binaries or want fresh writable filesystems, rerun the full one-time setup block above.

## 1. Multi-container supervision

###  Command
Terminal 1:
```bash
cd /boilerplate
sudo ./engine supervisor /boilerplate/rootfs-base
```


Terminal 2:
```bash
cd /boilerplate
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
sudo ./engine start alpha ./rootfs-alpha "sleep 60"
sudo ./engine start beta ./rootfs-beta "sleep 60"
sudo ./engine ps
```



### Screenshot
terminal 2
![alt text](image-14.png)

terminal 1
![alt text](image-2.png)

---

## 2. Metadata tracking

###  Command
```bash
cd /boilerplate
sudo ./engine ps
```





### Screenshot
![alt text](image-15.png)

---

## 3. Bounded-buffer logging

###  Command
```bash
cd /boilerplate
sudo ./engine run logdemo2 ./rootfs-alpha 'for i in 1 2 3 4 5; do echo log-$i; sleep 1; done'
sudo ./engine logs logdemo2
```



### Screenshot
Add screenshot here.
![alt text](image-4.png)
---

## 4. CLI and IPC

###  Command
```bash
cd /boilerplate
sudo ./engine start ipcstop ./rootfs-alpha "sleep 120"
sudo ./engine stop ipcstop
sudo ./engine ps
```


### Screenshot
![alt text](image-16.png)

---

## 5. Soft-limit warning

### Command
```bash
cd /boilerplate
chmod +x ./demo_steps.sh
./demo_steps.sh soft
```







### Screenshot
![alt text](image-17.png)
---

## 6. Hard-limit enforcement

### Command
```bash
cd /boilerplate
./demo_steps.sh hard
```




### Screenshot
![alt text](image-18.png)

---

## 7. Scheduling experiment


```bash
cd /boilerplate
./demo_steps.sh sched
```



### Screenshot
![alt text](image-12.png)

---

## 8. Clean teardown

### Copy-paste Command
```bash
cd /boilerplate
./demo_steps.sh teardown
```



### Screenshot
![alt text](image-19.png)

