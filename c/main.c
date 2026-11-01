/*
 * gamepad-mode-gui — 检测游戏手柄 USB 协议模式（HID / XInput / GIP 等）与振动能力。
 *
 * 用法:
 *   gamepad-mode-gui           # GTK3 图形界面（1s 自动刷新，支持热插拔）
 *   gamepad-mode-gui --text    # 一次性纯文本输出
 *   gamepad-mode-gui --json    # JSON 输出
 *
 * 仅依赖 GTK3（运行时）与标准 C 库。只读 /proc、/sys，无需 root。
 */

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEV   64
#define MAX_PATH  512
#define MAX_WORD  8

#define LEGEND \
    "提示：连接列为「虚拟」的条目是 Steam Input 用 uinput 生成的虚拟手柄" \
    "（VID 28de=Valve），游戏通过它转发输入/振动到真实手柄；" \
    "在 Steam 里关掉「启用 Steam 输入」后该条目会消失。"

typedef struct {
    char name[128];
    char vidpid[32];
    char vendor[8];
    char product[8];
    char transport[16];
    char mode[96];
    char driver[64];
    char rumble_label[24];
    int  rumble;
    int  ff;
} Gamepad;

typedef struct {
    char bus[8];
    char vendor[8];
    char product[8];
    char name[128];
    char phys[128];
    char sysfs[MAX_PATH];
    char handlers[64];
    unsigned long long abs_words[MAX_WORD];
    int abs_n;
    unsigned long long ff_words[MAX_WORD];
    int ff_n;
} RawDev;

/* ---------------- 基础工具 ---------------- */

static int read_file(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    out[strcspn(out, "\n")] = 0;
    return 0;
}

static void read_driver(const char *iface_dir, char *out, size_t n) {
    char path[MAX_PATH], link[MAX_PATH];
    snprintf(path, sizeof path, "%s/driver", iface_dir);
    ssize_t r = readlink(path, link, sizeof link - 1);
    if (r <= 0) { out[0] = 0; return; }
    link[r] = 0;
    const char *b = strrchr(link, '/');
    snprintf(out, n, "%s", b ? b + 1 : link);
}

static int bit_set(const unsigned long long *w, int n, int bit) {
    if (n <= 0 || bit < 0 || bit >= n * 64) return 0;
    int wi = (n - 1) - (bit / 64);
    int off = bit % 64;
    return (int)((w[wi] >> off) & 1ULL);
}

static void parse_bitmap(const char *s, unsigned long long *out, int *n) {
    *n = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    char *tok = strtok(buf, " ");
    while (tok && *n < MAX_WORD) {
        out[(*n)++] = strtoull(tok, NULL, 16);
        tok = strtok(NULL, " ");
    }
}

/* ---------------- /proc/bus/input/devices 解析 ---------------- */

static int parse_input_devices(RawDev *devs) {
    int count = 0;
    FILE *f = fopen("/proc/bus/input/devices", "r");
    if (!f) return 0;
    char line[1024];
    RawDev *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        if (strncmp(line, "I: ", 3) == 0) {
            if (count >= MAX_DEV) break;
            cur = &devs[count++];
            memset(cur, 0, sizeof *cur);
            char *tok = strtok(line + 3, " ");
            while (tok) {
                if (strncmp(tok, "Bus=", 4) == 0) snprintf(cur->bus, 8, "%s", tok + 4);
                else if (strncmp(tok, "Vendor=", 7) == 0) snprintf(cur->vendor, 8, "%s", tok + 7);
                else if (strncmp(tok, "Product=", 8) == 0) snprintf(cur->product, 8, "%s", tok + 8);
                tok = strtok(NULL, " ");
            }
        } else if (cur) {
            if (strncmp(line, "N: Name=", 8) == 0) {
                const char *v = line + 8;
                size_t l = strlen(v);
                if (l >= 2 && v[0] == '"' && v[l - 1] == '"') {
                    char tmp[128];
                    snprintf(tmp, sizeof tmp, "%.*s", (int)(l - 2), v + 1);
                    snprintf(cur->name, sizeof cur->name, "%s", tmp);
                } else {
                    snprintf(cur->name, sizeof cur->name, "%s", v);
                }
            } else if (strncmp(line, "P: Phys=", 8) == 0) {
                snprintf(cur->phys, sizeof cur->phys, "%s", line + 8);
            } else if (strncmp(line, "S: Sysfs=", 9) == 0) {
                snprintf(cur->sysfs, sizeof cur->sysfs, "%s", line + 9);
            } else if (strncmp(line, "H: Handlers=", 12) == 0) {
                snprintf(cur->handlers, sizeof cur->handlers, "%s", line + 12);
            } else if (strncmp(line, "B: ABS=", 7) == 0) {
                parse_bitmap(line + 7, cur->abs_words, &cur->abs_n);
            } else if (strncmp(line, "B: FF=", 6) == 0) {
                parse_bitmap(line + 6, cur->ff_words, &cur->ff_n);
            }
        }
    }
    fclose(f);
    return count;
}

/* ---------------- sysfs 协议解析 ---------------- */

static void resolve_interface(const char *sysfs_rel, char *cls, char *sub, char *proto, char *driver) {
    cls[0] = sub[0] = proto[0] = driver[0] = 0;
    char cur[MAX_PATH];
    if (strncmp(sysfs_rel, "/devices", 8) == 0)
        snprintf(cur, sizeof cur, "/sys%s", sysfs_rel);
    else
        snprintf(cur, sizeof cur, "%s", sysfs_rel);
    for (;;) {
        char *slash = strrchr(cur, '/');
        if (!slash) break;
        *slash = 0;
        if (cur[0] == 0) break;
        char t[MAX_PATH];
        snprintf(t, sizeof t, "%s/bInterfaceClass", cur);
        if (access(t, F_OK) == 0) {
            read_file(t, cls, 8);
            snprintf(t, sizeof t, "%s/bInterfaceSubClass", cur); read_file(t, sub, 8);
            snprintf(t, sizeof t, "%s/bInterfaceProtocol", cur); read_file(t, proto, 8);
            read_driver(cur, driver, 64);
            return;
        }
    }
}

static int is_virtual(const RawDev *d) {
    return d->phys[0] == 0 && strstr(d->sysfs, "virtual") != NULL;
}

static int is_gamepad(const RawDev *d) {
    if (!strstr(d->handlers, "js")) return 0;
    static const int codes[] = {2, 3, 4, 5, 16, 17};
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++)
        if (bit_set(d->abs_words, d->abs_n, codes[i])) return 1;
    for (int i = 0; i < d->ff_n; i++)
        if (d->ff_words[i]) return 1;
    char low[128];
    snprintf(low, sizeof low, "%s", d->name);
    for (char *p = low; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    return strstr(low, "pad") || strstr(low, "gamepad") || strstr(low, "controller")
        || strstr(low, "joystick") || strstr(low, "手柄");
}

static void classify(const RawDev *d, const char *cls, const char *sub,
                     const char *proto, const char *driver, Gamepad *g) {
    if (is_virtual(d)) {
        snprintf(g->mode, sizeof g->mode, "虚拟手柄（uinput/Steam）");
        return;
    }
    if (strcmp(d->bus, "0005") == 0 || strcmp(d->bus, "0006") == 0) {
        if (strcmp(driver, "hid-nintendo") == 0) snprintf(g->mode, sizeof g->mode, "Switch（HID）");
        else if (strcmp(driver, "hid-sony") == 0 || strcmp(driver, "hid-playstation") == 0) snprintf(g->mode, sizeof g->mode, "PlayStation（HID）");
        else if (strcmp(driver, "xpadneo") == 0) snprintf(g->mode, sizeof g->mode, "Xbox（蓝牙）");
        else snprintf(g->mode, sizeof g->mode, "HID（蓝牙）");
        return;
    }
    if (strcmp(cls, "03") == 0) {
        if (strcmp(driver, "hid-nintendo") == 0) snprintf(g->mode, sizeof g->mode, "Switch（HID）");
        else if (strcmp(driver, "hid-sony") == 0 || strcmp(driver, "hid-playstation") == 0) snprintf(g->mode, sizeof g->mode, "PlayStation（HID）");
        else snprintf(g->mode, sizeof g->mode, "HID（通用/安卓）");
        return;
    }
    if (strcmp(cls, "ff") == 0) {
        if (strcmp(sub, "47") == 0 && strcmp(proto, "d0") == 0) snprintf(g->mode, sizeof g->mode, "GIP（Xbox One / Xbox Gaming Device）");
        else if (strcmp(sub, "5d") == 0 && strcmp(proto, "01") == 0) snprintf(g->mode, sizeof g->mode, "XInput（Xbox 360）");
        else snprintf(g->mode, sizeof g->mode, "Xbox（厂商协议）");
        return;
    }
    snprintf(g->mode, sizeof g->mode, "其他（class %s）", cls[0] ? cls : "?");
}

/* ---------------- 扫描 ---------------- */

static int scan(Gamepad *out) {
    RawDev devs[MAX_DEV];
    int n = parse_input_devices(devs);
    int count = 0;
    for (int pass = 0; pass < 2; pass++) { /* pass 0: 实体, pass 1: 虚拟 */
        for (int i = 0; i < n; i++) {
            RawDev *d = &devs[i];
            if (!is_gamepad(d)) continue;
            if ((pass == 0) == is_virtual(d)) continue;
            Gamepad *g = &out[count++];
            memset(g, 0, sizeof *g);
            snprintf(g->name, sizeof g->name, "%s", d->name);
            snprintf(g->vendor, sizeof g->vendor, "%s", d->vendor);
            snprintf(g->product, sizeof g->product, "%s", d->product);
            snprintf(g->vidpid, sizeof g->vidpid, "%s:%s",
                     d->vendor[0] ? d->vendor : "?", d->product[0] ? d->product : "?");
            if (is_virtual(d)) snprintf(g->transport, sizeof g->transport, "虚拟");
            else if (strcmp(d->bus, "0003") == 0) snprintf(g->transport, sizeof g->transport, "USB");
            else if (strcmp(d->bus, "0005") == 0) snprintf(g->transport, sizeof g->transport, "蓝牙");
            else if (strcmp(d->bus, "0006") == 0) snprintf(g->transport, sizeof g->transport, "蓝牙(uhid)");
            else snprintf(g->transport, sizeof g->transport, "总线 %s", d->bus);

            char cls[8], sub[8], proto[8], driver[64];
            resolve_interface(d->sysfs, cls, sub, proto, driver);
            snprintf(g->driver, sizeof g->driver, "%s", driver);
            classify(d, cls, sub, proto, driver, g);

            int any_ff = 0;
            for (int k = 0; k < d->ff_n; k++) if (d->ff_words[k]) any_ff = 1;
            g->ff = any_ff;
            g->rumble = any_ff && bit_set(d->ff_words, d->ff_n, 0x50);
            if (g->rumble) snprintf(g->rumble_label, sizeof g->rumble_label, "有振动");
            else if (g->ff) snprintf(g->rumble_label, sizeof g->rumble_label, "有 FF（无 RUMBLE）");
            else snprintf(g->rumble_label, sizeof g->rumble_label, "无振动");
        }
    }
    return count;
}

/* ---------------- 文本 / JSON 输出 ---------------- */

static int strw(const char *s) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) w += (*p < 0x80) ? 1 : 2;
    return w;
}

static void print_text(void) {
    Gamepad pads[MAX_DEV];
    int n = scan(pads);
    if (n == 0) { printf("未检测到游戏手柄\n"); return; }
    const char *header[] = {"#", "名称", "VID:PID", "连接", "协议模式", "驱动", "振动"};
    char cell[MAX_DEV][7][128];
    int w[7] = {0};
    for (int c = 0; c < 7; c++) w[c] = strw(header[c]);
    for (int i = 0; i < n; i++) {
        Gamepad *p = &pads[i];
        snprintf(cell[i][0], 128, "%d", i + 1);
        snprintf(cell[i][1], 128, "%s", p->name);
        snprintf(cell[i][2], 128, "%s", p->vidpid);
        snprintf(cell[i][3], 128, "%s", p->transport);
        snprintf(cell[i][4], 128, "%s", p->mode);
        snprintf(cell[i][5], 128, "%s", p->driver[0] ? p->driver : "-");
        snprintf(cell[i][6], 128, "%s", p->rumble_label);
        for (int c = 0; c < 7; c++) { int x = strw(cell[i][c]); if (x > w[c]) w[c] = x; }
    }
    for (int c = 0; c < 7; c++) { fputs(header[c], stdout); for (int k = strw(header[c]); k < w[c] + 2; k++) putchar(' '); }
    putchar('\n');
    for (int i = 0; i < n; i++)
        for (int c = 0; c < 7; c++) {
            fputs(cell[i][c], stdout);
            int pad = w[c] - strw(cell[i][c]) + (c == 6 ? 0 : 2);
            for (int k = 0; k < pad; k++) putchar(' ');
            if (c == 6) putchar('\n');
        }
    printf("\n%s\n", LEGEND);
}

static void print_json(void) {
    Gamepad pads[MAX_DEV];
    int n = scan(pads);
    printf("[\n");
    for (int i = 0; i < n; i++) {
        Gamepad *p = &pads[i];
        printf("  {\n    \"name\": \"%s\",\n    \"vendor\": \"%s\",\n    \"product\": \"%s\",\n"
               "    \"transport\": \"%s\",\n    \"mode\": \"%s\",\n    \"driver\": \"%s\",\n"
               "    \"rumble\": %s,\n    \"ff\": %s\n  }%s\n",
               p->name, p->vendor, p->product, p->transport, p->mode, p->driver,
               p->rumble ? "true" : "false", p->ff ? "true" : "false",
               i == n - 1 ? "" : ",");
    }
    printf("]\n");
}

/* ---------------- GTK3 GUI ---------------- */

static GtkListStore *store;
static Gamepad g_pads[MAX_DEV];
static int g_n;

static void rescan(void) { g_n = scan(g_pads); }

static gboolean tick(gpointer data) {
    (void)data;
    rescan();
    gtk_list_store_clear(store);
    for (int i = 0; i < g_n; i++) {
        GtkTreeIter iter;
        Gamepad *p = &g_pads[i];
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
                           0, p->name, 1, p->vidpid, 2, p->transport,
                           3, p->mode, 4, p->driver, 5, p->rumble_label, -1);
    }
    return G_SOURCE_CONTINUE;
}

static void build_gui(void) {
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "gamepad-mode");
    gtk_window_set_default_size(GTK_WINDOW(win), 780, 240);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    store = gtk_list_store_new(6, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                               G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);
    const char *titles[] = {"名称", "VID:PID", "连接", "协议模式", "驱动", "振动"};
    for (int c = 0; c < 6; c++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(titles[c], r, "text", c, NULL);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);
        g_object_set(GTK_TREE_VIEW_COLUMN(col), "resizable", TRUE, NULL);
    }
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), tree);
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    GtkWidget *legend = gtk_label_new(LEGEND);
    gtk_label_set_line_wrap(GTK_LABEL(legend), TRUE);
    gtk_label_set_xalign(GTK_LABEL(legend), 0.0f);
    gtk_label_set_justify(GTK_LABEL(legend), GTK_JUSTIFY_LEFT);
    gtk_box_pack_start(GTK_BOX(vbox), legend, FALSE, FALSE, 4);

    gtk_widget_show_all(win);
    tick(NULL);
    g_timeout_add_seconds(1, tick, NULL);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--text") == 0) { print_text(); return 0; }
    if (argc > 1 && strcmp(argv[1], "--json") == 0) { print_json(); return 0; }
    gtk_init(&argc, &argv);
    build_gui();
    gtk_main();
    return 0;
}
