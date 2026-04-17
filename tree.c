// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── Forward declarations ────────────────────────────────────────────────────

// Forward declaration (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── TODO: Implement these ──────────────────────────────────────────────────

typedef struct {
    uint32_t mode;
    ObjectID hash;
    char path[512];
} TIndexEntry;

typedef struct {
    TIndexEntry entries[10000];
    int count;
} TIndex;

static int read_index_for_tree(TIndex *tidx) {
    if (!tidx) return -1;

    tidx->count = 0;
    FILE *fp = fopen(INDEX_FILE, "r");
    if (!fp) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        if (tidx->count >= (int)(sizeof(tidx->entries) / sizeof(tidx->entries[0]))) {
            fclose(fp);
            return -1;
        }

        TIndexEntry *ie = &tidx->entries[tidx->count];
        char hex_str[HASH_HEX_SIZE + 1];
        unsigned int mode;
        unsigned long long mtime_ignored;
        unsigned int size_ignored;
        char path[sizeof(ie->path)];

        if (sscanf(line, "%o %64s %llu %u %511[^\n]",
                   &mode, hex_str, &mtime_ignored, &size_ignored, path) != 5) {
            fclose(fp);
            return -1;
        }

        if (hex_to_hash(hex_str, &ie->hash) != 0) {
            fclose(fp);
            return -1;
        }

        ie->mode = mode;
        snprintf(ie->path, sizeof(ie->path), "%s", path);
        tidx->count++;
    }

    fclose(fp);
    return 0;
}

static int build_level(const TIndex *tidx, const char *prefix, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;

    size_t pfx_len = strlen(prefix);
    for (int i = 0; i < tidx->count; i++) {
        const char *path = tidx->entries[i].path;
        if (pfx_len > 0 && strncmp(path, prefix, pfx_len) != 0) continue;

        const char *rest = path + pfx_len;
        if (rest[0] == '\0') continue;

        const char *slash = strchr(rest, '/');
        if (!slash) {
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *entry = &tree.entries[tree.count++];
            entry->mode = tidx->entries[i].mode;
            entry->hash = tidx->entries[i].hash;
            snprintf(entry->name, sizeof(entry->name), "%s", rest);
            continue;
        }

        size_t dir_len = (size_t)(slash - rest);
        if (dir_len == 0 || dir_len >= 256) return -1;

        char dir_name[256];
        memcpy(dir_name, rest, dir_len);
        dir_name[dir_len] = '\0';

        int found = 0;
        for (int j = 0; j < tree.count; j++) {
            if (tree.entries[j].mode == MODE_DIR &&
                strcmp(tree.entries[j].name, dir_name) == 0) {
                found = 1;
                break;
            }
        }
        if (found) continue;

        char sub_prefix[1024];
        snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, dir_name);

        ObjectID sub_id;
        if (build_level(tidx, sub_prefix, &sub_id) != 0) return -1;

        if (tree.count >= MAX_TREE_ENTRIES) return -1;
        TreeEntry *entry = &tree.entries[tree.count++];
        entry->mode = MODE_DIR;
        entry->hash = sub_id;
        snprintf(entry->name, sizeof(entry->name), "%s", dir_name);
    }

    if (tree.count == 0) return -1;

    void *tdata = NULL;
    size_t tlen = 0;
    if (tree_serialize(&tree, &tdata, &tlen) != 0) return -1;

    int ret = object_write(OBJ_TREE, tdata, tlen, id_out);
    free(tdata);
    return ret;
}

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    if (!id_out) return -1;

    TIndex tidx;
    if (read_index_for_tree(&tidx) != 0) return -1;
    if (tidx.count == 0) return -1;

    return build_level(&tidx, "", id_out);
}
