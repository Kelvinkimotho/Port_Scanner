// port_monitor.c
// Cross-platform (Windows / Linux) simple port monitoring tool
// Menu-driven command-line GUI (no external dependencies)
// Features (best-effort):
//  - List listening TCP & UDP ports
//  - List active TCP connections
//  - Show PID and process name where available
//  - Filter by port or PID
//  - Export current view to CSV
//
// Build (Windows MSYS2/MinGW-w64):
//   gcc port_monitor.c -o port_monitor.exe -lws2_32 -liphlpapi -lpsapi
// Build (WSL / Linux):
//   gcc port_monitor.c -o port_monitor
//
// Run:
//   ./port_monitor   (or port_monitor.exe on Windows)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <psapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")
#else
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#endif

#define MAX_LINE 1024
#define CSV_FILENAME "port_monitor_export.csv"

typedef struct Entry {
    char proto[8]; // "TCP" or "UDP"
    char local_addr[64];
    unsigned int local_port;
    char remote_addr[64];
    unsigned int remote_port;
    char state[32];
    unsigned long pid; // 0 if unknown
    char proc_name[256];
} Entry;

typedef struct {
    Entry *data;
    size_t len;
    size_t cap;
} List;

static void list_init(List *l) { l->data = NULL; l->len = 0; l->cap = 0; }
static void list_free(List *l) { free(l->data); l->data = NULL; l->len = l->cap = 0; }
static void list_push(List *l, Entry *e) {
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->data = realloc(l->data, l->cap * sizeof(Entry));
    }
    l->data[l->len++] = *e;
}

static void clear_screen() {
    // ANSI clear
    printf("\x1b[2J\x1b[H");
}

static void pause_for_key() {
    printf("\nPress ENTER to continue...");
    fflush(stdout);
    while (getchar() != '\n');
}

#ifdef _WIN32
// Windows: use GetExtendedTcpTable / GetExtendedUdpTable to get connections
static void get_proc_name_from_pid(unsigned long pid, char *out, size_t outlen) {
    if (pid == 0) { strncpy(out, "-", outlen); return; }
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)pid);
    if (!h) { strncpy(out, "(access denied)", outlen); return; }
    HMODULE mods[1024]; DWORD needed;
    if (EnumProcessModules(h, mods, sizeof(mods), &needed)) {
        if (needed / sizeof(HMODULE) > 0) {
            if (GetModuleBaseNameA(h, mods[0], out, (DWORD)outlen) == 0) strncpy(out, "(name unk)", outlen);
        } else strncpy(out, "(no mods)", outlen);
    } else strncpy(out, "(enumfail)", outlen);
    CloseHandle(h);
}

static void collect_windows(List *list) {
    // TCP connections
    PMIB_TCPTABLE_OWNER_PID tcpTable = NULL;
    DWORD size = 0;
    DWORD res = GetExtendedTcpTable(NULL, &size, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    tcpTable = (PMIB_TCPTABLE_OWNER_PID)malloc(size);
    if (GetExtendedTcpTable(tcpTable, &size, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        for (DWORD i = 0; i < tcpTable->dwNumEntries; ++i) {
            MIB_TCPROW_OWNER_PID *r = &tcpTable->table[i];
            Entry e = {0};
            strcpy(e.proto, "TCP");
            struct in_addr a;
            a.S_un.S_addr = r->dwLocalAddr;
            snprintf(e.local_addr, sizeof(e.local_addr), "%s", inet_ntoa(a));
            e.local_port = ntohs((u_short)r->dwLocalPort);
            a.S_un.S_addr = r->dwRemoteAddr;
            snprintf(e.remote_addr, sizeof(e.remote_addr), "%s", inet_ntoa(a));
            e.remote_port = ntohs((u_short)r->dwRemotePort);
            switch (r->dwState) {
                case MIB_TCP_STATE_LISTEN: strcpy(e.state, "LISTEN"); break;
                case MIB_TCP_STATE_ESTAB: strcpy(e.state, "ESTABLISHED"); break;
                default: strcpy(e.state, "OTHER"); break;
            }
            e.pid = r->dwOwningPid;
            get_proc_name_from_pid(e.pid, e.proc_name, sizeof(e.proc_name));
            list_push(list, &e);
        }
    }
    free(tcpTable);

    // UDP table
    PMIB_UDPTABLE_OWNER_PID udpTable = NULL;
    size = 0;
    res = GetExtendedUdpTable(NULL, &size, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    udpTable = (PMIB_UDPTABLE_OWNER_PID)malloc(size);
    if (GetExtendedUdpTable(udpTable, &size, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
        for (DWORD i = 0; i < udpTable->dwNumEntries; ++i) {
            MIB_UDPROW_OWNER_PID *r = &udpTable->table[i];
            Entry e = {0};
            strcpy(e.proto, "UDP");
            struct in_addr a;
            a.S_un.S_addr = r->dwLocalAddr;
            snprintf(e.local_addr, sizeof(e.local_addr), "%s", inet_ntoa(a));
            e.local_port = ntohs((u_short)r->dwLocalPort);
            strcpy(e.remote_addr, "-"); e.remote_port = 0;
            strcpy(e.state, "LISTEN");
            e.pid = r->dwOwningPid;
            get_proc_name_from_pid(e.pid, e.proc_name, sizeof(e.proc_name));
            list_push(list, &e);
        }
    }
    free(udpTable);
}

#else
// Linux: parse /proc/net/tcp and /proc/net/udp; map inode -> pid by scanning /proc/*/fd

static int hex_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

static void addr_port_from_hex(const char *hexaddr, char *out_addr, size_t outlen, unsigned int *out_port) {
    // hexaddr format: "0100007F:0035" (IP little-endian)
    unsigned int ip_parts[4];
    unsigned int port;
    char iphex[9]; memset(iphex,0,sizeof(iphex));
    strncpy(iphex, hexaddr, 8);
    // IP is little endian 32-bit
    unsigned int ip = 0;
    for (int i = 0; i < 8; ++i) {
        ip = (ip << 4) | hex_to_int(iphex[i]);
    }
    // convert little endian to bytes
    unsigned char b[4];
    b[0] = (ip & 0xFF);
    b[1] = ((ip >> 8) & 0xFF);
    b[2] = ((ip >> 16) & 0xFF);
    b[3] = ((ip >> 24) & 0xFF);
    snprintf(out_addr, outlen, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    // port is after ':'
    const char *p = strchr(hexaddr, ':');
    if (!p) { *out_port = 0; return; }
    unsigned int val = 0; ++p;
    for (int i = 0; i < 4 && p[i]; ++i) {
        val = (val << 4) | hex_to_int(p[i]);
    }
    *out_port = val;
}

// Map socket inode to pid: scan /proc/<pid>/fd and readlink to see 'socket:[inode]'
static unsigned long inode_to_pid(const char *inode_str) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *de;
    char path[512];
    char link[512];
    while ((de = readdir(d)) != NULL) {
        if (!isdigit(de->d_name[0])) continue;
        snprintf(path, sizeof(path), "/proc/%s/fd", de->d_name);
        DIR *fd = opendir(path);
        if (!fd) continue;
        struct dirent *fde;
        while ((fde = readdir(fd)) != NULL) {
            if (fde->d_name[0] == '.') continue;
            char lnkpath[1024];
            snprintf(lnkpath, sizeof(lnkpath), "%s/%s", path, fde->d_name);
            ssize_t r = readlink(lnkpath, link, sizeof(link)-1);
            if (r > 0) {
                link[r] = '\0';
                // look for socket:[12345]
                char want[64]; snprintf(want, sizeof(want), "socket:[%s]", inode_str);
                if (strcmp(link, want) == 0) {
                    unsigned long pid = strtoul(de->d_name, NULL, 10);
                    closedir(fd);
                    closedir(d);
                    return pid;
                }
            }
        }
        closedir(fd);
    }
    closedir(d);
    return 0;
}

static void get_proc_name_from_pid(unsigned long pid, char *out, size_t outlen) {
    if (pid == 0) { strncpy(out, "-", outlen); return; }
    char path[256]; snprintf(path, sizeof(path), "/proc/%lu/comm", pid);
    FILE *f = fopen(path, "r");
    if (!f) { strncpy(out, "(access)", outlen); return; }
    if (fgets(out, outlen, f) == NULL) strncpy(out, "(readfail)", outlen);
    // strip newline
    out[strcspn(out, "\n")] = '\0';
    fclose(f);
}

static void parse_proc_net(const char *filename, const char *proto, List *list) {
    FILE *f = fopen(filename, "r");
    if (!f) return;
    char line[MAX_LINE];
    // skip header
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
    while (fgets(line, sizeof(line), f)) {
        // split
        char local[64], rem[64];
        char state[8];
        char inode[32];
        // Use sscanf with many fields safe parsing
        char rest[512];
        // This is a bit tolerant parsing
        int matched = sscanf(line, "%*d: %63s %63s %2s %*s %*s %*s %*s %*s %31s %511[\n]", local, rem, state, inode, rest);
        if (matched < 4) continue;
        Entry e = {0};
        strncpy(e.proto, proto, sizeof(e.proto)-1);
        addr_port_from_hex(local, e.local_addr, sizeof(e.local_addr), &e.local_port);
        addr_port_from_hex(rem, e.remote_addr, sizeof(e.remote_addr), &e.remote_port);
        // translate state
        if (strcmp(state, "0A") == 0) strncpy(e.state, "LISTEN", sizeof(e.state)-1);
        else if (strcmp(state, "01") == 0) strncpy(e.state, "ESTABLISHED", sizeof(e.state)-1);
        else strncpy(e.state, "OTHER", sizeof(e.state)-1);
        e.pid = inode_to_pid(inode);
        get_proc_name_from_pid(e.pid, e.proc_name, sizeof(e.proc_name));
        list_push(list, &e);
    }
    fclose(f);
}

static void collect_linux(List *list) {
    parse_proc_net("/proc/net/tcp", "TCP", list);
    parse_proc_net("/proc/net/udp", "UDP", list);
}
#endif

static void collect_system(List *list) {
    list_init(list);
#ifdef _WIN32
    // initialize Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return;
    }
    collect_windows(list);
    WSACleanup();
#else
    collect_linux(list);
#endif
}

static void print_entries(List *list, const char *filter) {
    printf("%-5s %-21s %-6s %-21s %-6s %-12s %-6s %-s\n",
           "Proto", "LocalAddr", "LPort", "RemoteAddr", "RPort", "State", "PID", "ProcName");
    for (size_t i = 0; i < list->len; ++i) {
        Entry *e = &list->data[i];
        if (filter && filter[0]) {
            // filter matches port or pid or proc name
            char buf[64]; snprintf(buf, sizeof(buf), "%u", e->local_port);
            if (strstr(buf, filter) == NULL && strstr(e->proc_name, filter) == NULL && strstr(e->state, filter) == NULL) continue;
        }
        printf("%-5s %-21s %-6u %-21s %-6u %-12s %-6lu %-s\n",
               e->proto, e->local_addr, e->local_port, e->remote_addr, e->remote_port, e->state, e->pid, e->proc_name);
    }
}

static void export_csv(List *list) {
    FILE *f = fopen(CSV_FILENAME, "w");
    if (!f) { fprintf(stderr, "Could not open %s for writing\n", CSV_FILENAME); return; }
    fprintf(f, "proto,local_addr,local_port,remote_addr,remote_port,state,pid,proc_name\n");
    for (size_t i = 0; i < list->len; ++i) {
        Entry *e = &list->data[i];
        fprintf(f, "%s,%s,%u,%s,%u,%s,%lu,%s\n",
                e->proto, e->local_addr, e->local_port, e->remote_addr, e->remote_port, e->state, e->pid, e->proc_name);
    }
    fclose(f);
    printf("Exported %zu entries to %s\n", list->len, CSV_FILENAME);
}

int main(void) {
    List entries;
    char input[128];
    char filter[128] = "";
    while (1) {
        clear_screen();
        printf("Port Monitor — Menu-driven CLI\n");
        printf("Generated: %s\n\n", __DATE__ " " __TIME__);
        printf("1) Refresh and show all ports\n");
        printf("2) Show active connections only (TCP ESTABLISHED)\n");
        printf("3) Filter (enter port, pid or substring of process name/state) [current: '%s']\n", filter);
        printf("4) Export current view to CSV (%s)\n", CSV_FILENAME);
        printf("5) Clear filter\n");
        printf("6) Quit\n");
        printf("\nSelect option: ");
        if (!fgets(input, sizeof(input), stdin)) break;
        int opt = atoi(input);
        if (opt == 6) break;
        // collect
        list_free(&entries); 
        list_init(&entries);
        collect_system(&entries);
        if (opt == 1) {
            print_entries(&entries, filter);
            pause_for_key();
        } else if (opt == 2) {
            // print only TCP ESTABLISHED
            List sub; list_init(&sub);
            for (size_t i = 0; i < entries.len; ++i) {
                Entry *e = &entries.data[i];
                if (strcmp(e->proto, "TCP") == 0 && strcmp(e->state, "ESTABLISHED") == 0) list_push(&sub, e);
            }
            print_entries(&sub, filter);
            list_free(&sub);
            pause_for_key();
        } else if (opt == 3) {
            printf("Enter filter (port, pid, or proc substring): ");
            if (!fgets(filter, sizeof(filter), stdin)) filter[0] = '\0';
            filter[strcspn(filter, "\n")] = '\0';
            print_entries(&entries, filter);
            pause_for_key();
        } else if (opt == 4) {
            export_csv(&entries);
            pause_for_key();
        } else if (opt == 5) {
            filter[0] = '\0';
            printf("Filter cleared.\n");
            pause_for_key();
        } else {
            printf("Unknown option.\n"); pause_for_key();
        }
        list_free(&entries);
    }
    printf("Bye.\n");
    return 0;
}
