/* AnkaOS - RAM dosya sistemi: ağaç yapılı, dizin + dosya, çekirdek heap'inde yaşar.
 * Dizin çocukları ada göre sıralı tutulur. /bin altına gömülü programlar kopyalanır. */
#include <kernel.h>

static fsnode_t *root;

static fsnode_t *node_new(const char *name, int type)
{
    fsnode_t *n = kmalloc(sizeof *n);
    if (!n) return NULL;
    memset(n, 0, sizeof *n);
    strncpy(n->name, name, FS_NAME_MAX);
    n->type = (u8)type;
    return n;
}

fsnode_t *fs_root(void) { return root; }

static fsnode_t *child_find(fsnode_t *dir, const char *name)
{
    for (fsnode_t *c = dir->child; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

static void link_child(fsnode_t *dir, fsnode_t *n)
{
    fsnode_t **pp = &dir->child;
    while (*pp && strcmp((*pp)->name, n->name) < 0) pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
    n->parent = dir;
}

static void unlink_child(fsnode_t *n)
{
    fsnode_t **pp = &n->parent->child;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
    n->next = NULL;
}

/* yolu bileşen bileşen gez ("." ve ".." desteklenir) */
fsnode_t *fs_resolve(fsnode_t *cwd, const char *path)
{
    fsnode_t *cur = (path[0] == '/') ? root : (cwd ? cwd : root);
    char comp[FS_NAME_MAX + 2];
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        int n = 0;
        while (path[n] && path[n] != '/') n++;
        if (n > FS_NAME_MAX) return NULL;
        memcpy(comp, path, (size_t)n);
        comp[n] = 0;
        path += n;
        if (cur->type != FS_DIR) return NULL;
        if (strcmp(comp, ".") == 0) continue;
        if (strcmp(comp, "..") == 0) { if (cur->parent) cur = cur->parent; continue; }
        cur = child_find(cur, comp);
        if (!cur) return NULL;
    }
    return cur;
}

/* yolu (ebeveyn dizin, son bileşen adı) olarak ayır */
int fs_split(fsnode_t *cwd, const char *path, fsnode_t **dir, char *name)
{
    int len = (int)strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    if (len == 0) return FS_EINVAL;
    int i = len;
    while (i > 0 && path[i - 1] != '/') i--;
    int nlen = len - i;
    if (nlen == 0) return FS_EINVAL;
    if (nlen > FS_NAME_MAX) return FS_ENAMETOOLONG;
    memcpy(name, path + i, (size_t)nlen);
    name[nlen] = 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return FS_EINVAL;

    char parent[128];
    if (i == 0) {
        strcpy(parent, ".");
    } else {
        if (i >= (int)sizeof parent) return FS_ENAMETOOLONG;
        memcpy(parent, path, (size_t)i);
        parent[i] = 0;
    }
    fsnode_t *d = fs_resolve(cwd, parent);
    if (!d) return FS_ENOENT;
    if (d->type != FS_DIR) return FS_ENOTDIR;
    *dir = d;
    return FS_OK;
}

int fs_create(fsnode_t *cwd, const char *path, int type, fsnode_t **out)
{
    fsnode_t *dir;
    char name[FS_NAME_MAX + 1];
    int r = fs_split(cwd, path, &dir, name);
    if (r) return r;
    if (child_find(dir, name)) return FS_EEXIST;
    fsnode_t *n = node_new(name, type);
    if (!n) return FS_ENOMEM;
    link_child(dir, n);
    if (out) *out = n;
    return FS_OK;
}

int fs_read(fsnode_t *f, u32 off, void *buf, u32 len)
{
    if (f->type != FS_FILE) return FS_EISDIR;
    if (off >= f->size) return 0;
    if (len > f->size - off) len = f->size - off;
    memcpy(buf, f->data + off, len);
    return (int)len;
}

int fs_write(fsnode_t *f, u32 off, const void *buf, u32 len)
{
    if (f->type != FS_FILE) return FS_EISDIR;
    u32 need = off + len;
    if (need > f->cap) {
        u32 ncap = f->cap ? f->cap * 2 : 64;
        while (ncap < need) ncap *= 2;
        u8 *nd = krealloc(f->data, ncap);
        if (!nd) return FS_ENOMEM;
        f->data = nd;
        f->cap = ncap;
    }
    if (off > f->size) memset(f->data + f->size, 0, off - f->size);
    memcpy(f->data + off, buf, len);
    if (need > f->size) f->size = need;
    return (int)len;
}

void fs_truncate(fsnode_t *f)
{
    kfree(f->data);
    f->data = NULL;
    f->size = f->cap = 0;
}

int fs_remove(fsnode_t *n)
{
    if (n == root) return FS_EBUSY;
    if (n->type == FS_DIR && n->child) return FS_ENOTEMPTY;
    unlink_child(n);
    kfree(n->data);
    kfree(n);
    return FS_OK;
}

int fs_move(fsnode_t *n, fsnode_t *np, const char *newname)
{
    if (n == root) return FS_EBUSY;
    if (strlen(newname) > FS_NAME_MAX) return FS_ENAMETOOLONG;
    for (fsnode_t *p = np; p; p = p->parent)            /* kendi altına taşıma yok */
        if (p == n) return FS_EINVAL;
    fsnode_t *ex = child_find(np, newname);
    if (ex && ex != n) return FS_EEXIST;
    unlink_child(n);
    strncpy(n->name, newname, FS_NAME_MAX);
    n->name[FS_NAME_MAX] = 0;
    link_child(np, n);
    return FS_OK;
}

void fs_path(fsnode_t *n, char *buf, int size)
{
    if (n == root) { ksnprintf(buf, (size_t)size, "/"); return; }
    char tmp[128];
    int pos = (int)sizeof tmp - 1;
    tmp[pos] = 0;
    for (; n && n != root; n = n->parent) {
        int l = (int)strlen(n->name);
        if (pos - l - 1 < 0) break;
        pos -= l;
        memcpy(tmp + pos, n->name, (size_t)l);
        tmp[--pos] = '/';
    }
    ksnprintf(buf, (size_t)size, "%s", tmp + pos);
}

const char *fs_strerror(int err)
{
    switch (err) {
    case FS_ENOENT:       return "no such file or directory";
    case FS_ENOMEM:       return "out of memory";
    case FS_EEXIST:       return "already exists";
    case FS_ENOTDIR:      return "not a directory";
    case FS_EISDIR:       return "is a directory";
    case FS_EINVAL:       return "invalid argument";
    case FS_ENAMETOOLONG: return "name too long";
    case FS_ENOTEMPTY:    return "directory not empty";
    case FS_EBUSY:        return "busy";
    default:              return "error";
    }
}

static void put_file(const char *path, const void *data, u32 len)
{
    fsnode_t *f;
    if (fs_create(root, path, FS_FILE, &f) == FS_OK && len) fs_write(f, 0, data, len);
}

void fs_init(void)
{
    root = node_new("/", FS_DIR);
    if (!root) kpanic("fs_init: out of memory");
    const char *dirs[] = { "/bin", "/etc", "/home", "/tmp" };
    for (u32 i = 0; i < ARRAY_SIZE(dirs); i++) fs_create(root, dirs[i], FS_DIR, NULL);

    for (int i = 0; i < nprogs; i++) {
        char path[48];
        ksnprintf(path, sizeof path, "/bin/%s", progs[i].name);
        put_file(path, progs[i].start, (u32)(progs[i].end - progs[i].start));
    }

    static const char motd[] =
        "Welcome to AnkaOS - a small x86 operating system written in C and assembly.\n"
        "Type 'help' for the command list. Try: hello, primes 5000 &, guess, crash kernel\n";
    put_file("/etc/motd", motd, sizeof motd - 1);

    static const char readme[] =
        "AnkaOS demo files live in RAM. Use echo/cat/cp/mv/rm to play with them:\n"
        "  echo hello > /tmp/a.txt\n  cat /tmp/a.txt\n  ls /bin\n";
    put_file("/home/readme.txt", readme, sizeof readme - 1);
}
