#include "tree.h"
#include "index.h"
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;

    const uint8_t *ptr = data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *e = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_buf[16] = {0};
        size_t mlen = space - ptr;
        if (mlen >= sizeof(mode_buf)) return -1;

        memcpy(mode_buf, ptr, mlen);
        e->mode = strtol(mode_buf, NULL, 8);

        ptr = space + 1;

        const uint8_t *null = memchr(ptr, '\0', end - ptr);
        if (!null) return -1;

        size_t nlen = null - ptr;
        if (nlen >= sizeof(e->name)) return -1;

        memcpy(e->name, ptr, nlen);
        e->name[nlen] = '\0';

        ptr = null + 1;

        if (ptr + HASH_SIZE > end) return -1;

        memcpy(e->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }

    return 0;
}

static int cmp_entries(const void *a, const void *b) {
    return strcmp(((TreeEntry *)a)->name, ((TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max = tree->count * 296;

    uint8_t *buf = malloc(max);
    if (!buf) return -1;

    Tree sorted = *tree;
    qsort(sorted.entries, sorted.count, sizeof(TreeEntry), cmp_entries);

    size_t off = 0;

    for (int i = 0; i < sorted.count; i++) {
        const TreeEntry *e = &sorted.entries[i];

        int w = snprintf((char *)buf + off, max - off, "%o %s", e->mode, e->name);
        if (w < 0) { free(buf); return -1; }

        off += w + 1;

        memcpy(buf + off, e->hash.hash, HASH_SIZE);
        off += HASH_SIZE;
    }

    *data_out = buf;
    *len_out = off;
    return 0;
}

/* ───────────── TREE BUILDING ───────────── */

static int build_tree(Index *index, const char *prefix, ObjectID *out_id) {
    Tree tree;
    tree.count = 0;

    char seen[1024][256];
    int seen_count = 0;

    size_t plen = strlen(prefix);

    for (int i = 0; i < index->count; i++) {
        const char *path = index->entries[i].path;

        if (strncmp(path, prefix, plen) != 0)
            continue;

        const char *rest = path + plen;
        const char *slash = strchr(rest, '/');

        if (!slash) {
            if (tree.count >= MAX_TREE_ENTRIES) return -1;

            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = index->entries[i].mode;
            e->hash = index->entries[i].hash;
            strcpy(e->name, rest);
        } else {
            size_t len = slash - rest;
            if (len >= 256) return -1;

            char dirname[256];
            memcpy(dirname, rest, len);
            dirname[len] = '\0';

            int exists = 0;
            for (int j = 0; j < seen_count; j++) {
                if (strcmp(seen[j], dirname) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (exists) continue;

            strcpy(seen[seen_count++], dirname);

            char new_prefix[512];
            snprintf(new_prefix, sizeof(new_prefix), "%s%s/", prefix, dirname);

            ObjectID sub;
            if (build_tree(index, new_prefix, &sub) != 0)
                return -1;

            if (tree.count >= MAX_TREE_ENTRIES) return -1;

            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = MODE_DIR;
            e->hash = sub;
            strcpy(e->name, dirname);
        }
    }

    void *data;
    size_t len;

    if (tree_serialize(&tree, &data, &len) != 0)
        return -1;

    int rc = object_write(OBJ_TREE, data, len, out_id);
    free(data);

    return rc;
}

int tree_from_index(ObjectID *id_out) {
    Index index;
    if (index_load(&index) != 0)
        return -1;

    return build_tree(&index, "", id_out);
}
