## 🧩 PORT MONITOR
A single-file, menu-driven C port monitoring tool for Windows and Linux.
Lists TCP/UDP sockets, active connections, PIDs, and process names — right from your terminal.
## ⚙️ FEATURES
• List listening TCP & UDP ports
• Show active TCP connections (ESTABLISHED)
• Display PID and process name (when available)
• Filter by port, PID, process name, or connection state
• Export visible entries to CSV (port_monitor_export.csv)

## 🧱 BUILD INSTRUCTIONS
### 🐧 LINUX
 `sudo apt update`
  `sudo apt install build-essential`
   `gcc port_monitor.c -o port_monitor`

###  WINDOWS (MSYS2 / MinGW-w64)
  `pacman -Syu`
   `pacman -S mingw-w64-x86_64-gcc`
    `gcc port_monitor.c -o port_monitor.exe -lws2_32 -liphlpapi -lpsapi`
## 🧭 USAGE
# Linux
  `./port_monitor`

# Windows
   `port_monitor.exe`
## 🧮 MENU OVERVIEW
1) Refresh and show all ports
2) Show active connections only (TCP ESTABLISHED)
3) Filter (by port, PID, process name, or state)
4) Export current view to CSV
5) Clear filter
6) Quit
## 🔍 FILTERING
Filter string matches any of:
   - Local port number (as text)
   - PID
   - Process name substring
   - Connection state (LISTEN, ESTABLISHED, etc.)
Use option 5 to clear the filter.
## 📤 EXPORTING TO CSV
Select option 4 to export current entries to:
    port_monitor_export.csv
## 💡 EXAMPLE SESSION
  `./port_monitor`


Select: 1
→ Showing only entries involving 93.184.216.34

Select: 4
→ Exported to port_monitor_export.csv
## ⚠️ LIMITATIONS
• IPv4 only (no IPv6 support)
• On Linux, reading /proc/<pid>/fd may need root
• Windows requires permission to read process info
• Parsing is best-effort (depends on OS version)
• No external dependencies — single source file
## 🧰 QUICK BUILD & RUN
gcc port_monitor.c -o port_monitor
sudo ./port_monitor

## UI

![Main Menu](./screenshots/one.png)

![All Ports](./screenshots/two.png)

![Established](./screenshots/three.png)

![Set Target](./screenshots/four.png)

![Export CSV](./screenshots/five.png)

