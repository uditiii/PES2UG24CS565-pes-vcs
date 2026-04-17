// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
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

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    if (!id_out) return -1;
    if (!data && len > 0) return -1;

    const char *type_name;
    switch (type) {
        case OBJ_BLOB:   type_name = "blob";   break;
        case OBJ_TREE:   type_name = "tree";   break;
        case OBJ_COMMIT: type_name = "commit"; break;
        default: return -1;
    }

    char hdr_buf[64];
    int hdr_written = snprintf(hdr_buf, sizeof(hdr_buf), "%s %zu", type_name, len);
    if (hdr_written < 0 || (size_t)hdr_written >= sizeof(hdr_buf) - 1) return -1;
    size_t hdr_len = (size_t)hdr_written + 1;

    size_t obj_len = hdr_len + len;
    uint8_t *obj_buf = malloc(obj_len);
    if (!obj_buf) return -1;
    memcpy(obj_buf, hdr_buf, hdr_len);
    if (len > 0) memcpy(obj_buf + hdr_len, data, len);

    compute_hash(obj_buf, obj_len, id_out);
    if (object_exists(id_out)) {
        free(obj_buf);
        return 0;
    }

    if (mkdir(OBJECTS_DIR, 0755) != 0 && errno != EEXIST) {
        free(obj_buf);
        return -1;
    }

    char hex_str[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex_str);

    char shard_path[512];
    snprintf(shard_path, sizeof(shard_path), "%s/%.2s", OBJECTS_DIR, hex_str);
    if (mkdir(shard_path, 0755) != 0 && errno != EEXIST) {
        free(obj_buf);
        return -1;
    }

    char obj_path[512];
    object_path(id_out, obj_path, sizeof(obj_path));

    char tmp_file[640];
    snprintf(tmp_file, sizeof(tmp_file), "%s/.tmp-XXXXXX", shard_path);

    int fd = mkstemp(tmp_file);
    if (fd < 0) {
        free(obj_buf);
        return -1;
    }

    size_t bytes_written = 0;
    while (bytes_written < obj_len) {
        ssize_t n = write(fd, obj_buf + bytes_written, obj_len - bytes_written);
        if (n < 0) {
            close(fd);
            unlink(tmp_file);
            free(obj_buf);
            return -1;
        }
        bytes_written += (size_t)n;
    }

    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_file);
        free(obj_buf);
        return -1;
    }

    if (close(fd) != 0) {
        unlink(tmp_file);
        free(obj_buf);
        return -1;
    }

    if (rename(tmp_file, obj_path) != 0) {
        unlink(tmp_file);
        free(obj_buf);
        return -1;
    }

    int dir_fd = open(shard_path, O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        (void)fsync(dir_fd);
        close(dir_fd);
    }

    free(obj_buf);
    return 0;
}

// Read an object from the store.
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    if (!id || !type_out || !data_out || !len_out) return -1;

    char path[512];
    object_path(id, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long file_size = ftell(fp);
    if (file_size < 0) { fclose(fp); return -1; }
    rewind(fp);

    uint8_t *raw_buf = malloc((size_t)file_size);
    if (!raw_buf) { fclose(fp); return -1; }

    if (fread(raw_buf, 1, (size_t)file_size, fp) != (size_t)file_size) {
        free(raw_buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    uint8_t *sep = memchr(raw_buf, '\0', (size_t)file_size);
    if (!sep) { free(raw_buf); return -1; }

    char hdr[64];
    size_t hdr_len = (size_t)(sep - raw_buf);
    if (hdr_len >= sizeof(hdr)) { free(raw_buf); return -1; }
    memcpy(hdr, raw_buf, hdr_len);
    hdr[hdr_len] = '\0';

    char tname[16];
    size_t expected_size = 0;
    if (sscanf(hdr, "%15s %zu", tname, &expected_size) != 2) { free(raw_buf); return -1; }

    if (strcmp(tname, "blob") == 0)        *type_out = OBJ_BLOB;
    else if (strcmp(tname, "tree") == 0)   *type_out = OBJ_TREE;
    else if (strcmp(tname, "commit") == 0) *type_out = OBJ_COMMIT;
    else { free(raw_buf); return -1; }

    size_t payload_len = (size_t)file_size - hdr_len - 1;
    if (payload_len != expected_size) { free(raw_buf); return -1; }

    ObjectID result_id;
    compute_hash(raw_buf, (size_t)file_size, &result_id);
    if (memcmp(&result_id, id, sizeof(ObjectID)) != 0) { free(raw_buf); return -1; }

    uint8_t *payload = malloc(payload_len > 0 ? payload_len : 1);
    if (!payload) { free(raw_buf); return -1; }
    if (payload_len > 0) memcpy(payload, sep + 1, payload_len);

    *data_out = payload;
    *len_out = payload_len;

    free(raw_buf);
    return 0;
}
