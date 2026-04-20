#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

/* ─── PROVIDED ───────────────────────────────────────────────────────── */

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++)
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;

    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1)
            return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();

    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);

    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size,
             "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

/* ─── IMPLEMENTED ───────────────────────────────────────────────────── */

int object_write(ObjectType type, const void *data,
                 size_t len, ObjectID *id_out) {

    const char *type_str =
        (type == OBJ_BLOB) ? "blob" :
        (type == OBJ_TREE) ? "tree" : "commit";

    char header[64];
    int header_len =
        snprintf(header, sizeof(header),
                 "%s %zu", type_str, len) + 1;

    size_t total = header_len + len;

    char *buffer = malloc(total);
    if (!buffer) return -1;

    memcpy(buffer, header, header_len);
    memcpy(buffer + header_len, data, len);

    compute_hash(buffer, total, id_out);

    if (object_exists(id_out)) {
        free(buffer);
        return 0;
    }

    char path[512];
    object_path(id_out, path, sizeof(path));

    char dir[512];
    strncpy(dir, path, sizeof(dir));
    char *slash = strrchr(dir, '/');
    *slash = '\0';

    mkdir(OBJECTS_DIR, 0755);
    mkdir(dir, 0755);

    char temp[520];
    snprintf(temp, sizeof(temp), "%s.tmp", path);

    int fd = open(temp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        free(buffer);
        return -1;
    }

    ssize_t written = write(fd, buffer, total);
    if (written != (ssize_t)total) {
        close(fd);
        free(buffer);
        return -1;
    }

    fsync(fd);
    close(fd);

    if (rename(temp, path) != 0) {
        free(buffer);
        return -1;
    }

    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }

    free(buffer);
    return 0;
}

/* ───────────────────────────────────────────────────────────────────── */

int object_read(const ObjectID *id,
                ObjectType *type_out,
                void **data_out,
                size_t *len_out) {

    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    rewind(f);

    char *buffer = malloc(size);
    if (!buffer) {
        fclose(f);
        return -1;
    }

    fread(buffer, 1, size, f);
    fclose(f);

    ObjectID check;
    compute_hash(buffer, size, &check);

    if (memcmp(check.hash, id->hash, HASH_SIZE) != 0) {
        free(buffer);
        return -1;
    }

    char *space = memchr(buffer, ' ', size);
    char *nullb = memchr(buffer, '\0', size);

    if (!space || !nullb) {
        free(buffer);
        return -1;
    }

    if (strncmp(buffer, "blob", 4) == 0)
        *type_out = OBJ_BLOB;
    else if (strncmp(buffer, "tree", 4) == 0)
        *type_out = OBJ_TREE;
    else
        *type_out = OBJ_COMMIT;

    *len_out = atoi(space + 1);

    *data_out = malloc(*len_out);
    if (!*data_out) {
        free(buffer);
        return -1;
    }

    memcpy(*data_out, nullb + 1, *len_out);

    free(buffer);
    return 0;
}
