
# Screenshot Evidence Template

Use this file to record each required screenshot item.
For every section:
- Run the command(s)
- Paste the real output under Output
- Add your screenshot image link later

## 1. Multi-container supervision

### Command
Terminal 1:
```bash
cd /home/angelo/osprt2/OS-Jackfruit/boilerplate
sudo ./engine supervisor /home/angelo/osprt2/OS-Jackfruit/boilerplate/rootfs-base
```

Terminal 2:
```bash
cd /home/angelo/osprt2/OS-Jackfruit/boilerplate
cp -a rootfs-base rootfs-alpha
cp -a rootfs-base rootfs-beta
sudo ./engine start alpha ./rootfs-alpha "sleep 60"
sudo ./engine start beta ./rootfs-beta "sleep 60"
sudo ./engine ps
```

### Output
Paste terminal output here.

### Screenshot
Add screenshot here.

---

## 2. Metadata tracking

### Command
```bash
cd /home/angelo/osprt2/OS-Jackfruit/boilerplate
sudo ./engine ps
```

### Output
Paste terminal output here.

### Screenshot
Add screenshot here.

---

## 3. Bounded-buffer logging

### Command
```bash
cd /home/angelo/osprt2/OS-Jackfruit/boilerplate
sudo ./engine run logdemo ./rootfs-alpha "for i in 1 2 3 4 5; do echo log-$i; sleep 1; done"
sudo ./engine logs logdemo
```

### Output
Paste terminal output here.

### Screenshot
Add screenshot here.

---

## 4. CLI and IPC

### Command
```bash
cd /home/angelo/osprt2/OS-Jackfruit/boilerplate
sudo ./engine stop alpha
```

### Output
Paste terminal output here.

### Screenshot
Add screenshot here.

---

## 5. Soft-limit warning

### Command
```bash
cd /home/angelo/osprt2/OS-Jackfruit/boilerplate
make module
sudo insmod monitor.ko
ls -l /dev/container_monitor
cp memory_hog rootfs-alpha/
sudo ./engine start softtest ./rootfs-alpha "/memory_hog" --soft-mib 32 --hard-mib 96
dmesg | tail -n 20
```

### Output
Paste terminal output here.

### Screenshot
Add screenshot here.

---

## 6. Hard-limit enforcement

### Command
```bash
cd /home/angelo/osprt2/OS-Jackfruit/boilerplate
cp memory_hog rootfs-beta/
sudo ./engine start hardtest ./rootfs-beta "/memory_hog" --soft-mib 32 --hard-mib 48
dmesg | tail -n 20
sudo ./engine ps
```

### Output
Paste terminal output here.

### Screenshot
Add screenshot here.

---

## 7. Scheduling experiment

### Command
```bash
cd /home/angelo/osprt2/OS-Jackfruit/boilerplate
cp cpu_hog rootfs-alpha/
cp cpu_hog rootfs-beta/
time sudo ./engine run cpu-low ./rootfs-alpha "/cpu_hog"
time sudo ./engine run cpu-high ./rootfs-beta "/cpu_hog"
```

### Output
Paste terminal output here.

### Screenshot
Add screenshot here.

---

## 8. Clean teardown

### Command
```bash
cd /home/angelo/osprt2/OS-Jackfruit/boilerplate
sudo ./engine stop alpha
sudo ./engine stop beta
sudo ./engine ps
ps -ef | grep '[d]efunct'
sudo rmmod monitor
```

### Output
Paste terminal output here.

### Screenshot
Add screenshot here.

