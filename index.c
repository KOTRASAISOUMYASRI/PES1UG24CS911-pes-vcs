#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

/* ─── PROVIDED ───────────────────────────────────────────────────────────── */

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int rem = index->count - i - 1;
            if (rem > 0)
                memmove(&index->entries[i],
                        &index->entries[i + 1],
                        rem * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    if (index->count == 0) printf("  (nothing to show)\n");
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
    }
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged++;
            }
        }
    }
    if (unstaged == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked = 0;

    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {

            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0 ||
                strcmp(ent->d_name, ".pes") == 0 ||
                strcmp(ent->d_name, "pes") == 0 ||
                strstr(ent->d_name, ".o"))
                continue;

            int tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    tracked = 1;
                    break;
                }
            }

            if (!tracked) {
                struct stat st;
                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked++;
                }
            }
        }
        closedir(dir);
    }

    if (untracked == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

/* ─── IMPLEMENTED ───────────────────────────────────────────────────────── */

int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    while (index->count < MAX_INDEX_ENTRIES) {
        IndexEntry *e = &index->entries[index->count];

        char hash_hex[HASH_HEX_SIZE + 1];

        if (fscanf(f, "%o %64s %llu %u %[^\n]\n",
                   &e->mode,
                   hash_hex,
                   &e->mtime_sec,
                   &e->size,
                   e->path) != 5)
            break;

        if (hex_to_hash(hash_hex, &e->hash) != 0) {
            fclose(f);
            return -1;
        }

        index->count++;
    }

    fclose(f);
    return 0;
}

/* ───────────────────────────────────────────────────────────────────────── */

static int cmp_entries(const void *a, const void *b) {
    return strcmp(((IndexEntry *)a)->path,
                  ((IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    Index sorted = *index;
    qsort(sorted.entries, sorted.count,
          sizeof(IndexEntry), cmp_entries);

    char temp[] = ".pes/index.tmp";

    FILE *f = fopen(temp, "w");
    if (!f) return -1;

    for (int i = 0; i < sorted.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted.entries[i].hash, hex);

        fprintf(f, "%o %s %llu %u %s\n",
                sorted.entries[i].mode,
                hex,
                sorted.entries[i].mtime_sec,
                sorted.entries[i].size,
                sorted.entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    if (rename(temp, INDEX_FILE) != 0)
        return -1;

    return 0;
}

/* ───────────────────────────────────────────────────────────────────────── */

int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    char *data = malloc(st.st_size);
    if (!data) {
        fclose(f);
        return -1;
    }

    fread(data, 1, st.st_size, f);
    fclose(f);

    ObjectID id;
    if (object_write(OBJ_BLOB, data, st.st_size, &id) != 0) {
        free(data);
        return -1;
    }

    free(data);

    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES)
            return -1;
        e = &index->entries[index->count++];
    }

    e->mode = st.st_mode;
    e->hash = id;
    e->mtime_sec = st.st_mtime;
    e->size = st.st_size;
    strcpy(e->path, path);

    return index_save(index);
}
