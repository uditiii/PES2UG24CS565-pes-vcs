// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// Forward declaration (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

static int cmp_index_paths(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

// Load the index from .pes/index.
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    if (!index) return -1;

    index->count = 0;
    FILE *fp = fopen(INDEX_FILE, "r");
    if (!fp) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    char line[2048];
    while (fgets(line, sizeof(line), fp)) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fclose(fp);
            return -1;
        }

        IndexEntry *ie = &index->entries[index->count];
        char hex_str[HASH_HEX_SIZE + 1];
        unsigned int mode;
        unsigned long long mtime;
        unsigned int sz;
        char path[sizeof(ie->path)];

        if (sscanf(line, "%o %64s %llu %u %511[^\n]",
                   &mode, hex_str, &mtime, &sz, path) != 5) {
            fclose(fp);
            return -1;
        }

        if (hex_to_hash(hex_str, &ie->hash) != 0) {
            fclose(fp);
            return -1;
        }

        ie->mode = mode;
        ie->mtime_sec = (uint64_t)mtime;
        ie->size = sz;
        snprintf(ie->path, sizeof(ie->path), "%s", path);
        index->count++;
    }

    fclose(fp);
    return 0;
}

// Save the index to .pes/index atomically.
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    if (!index) return -1;

    IndexEntry *tmp_entries = NULL;
    if (index->count > 0) {
        tmp_entries = malloc((size_t)index->count * sizeof(IndexEntry));
        if (!tmp_entries) return -1;
        memcpy(tmp_entries, index->entries, (size_t)index->count * sizeof(IndexEntry));
        qsort(tmp_entries, index->count, sizeof(IndexEntry), cmp_index_paths);
    }

    char tmp_idx_path[512];
    snprintf(tmp_idx_path, sizeof(tmp_idx_path), "%s.tmp", INDEX_FILE);

    FILE *fp = fopen(tmp_idx_path, "w");
    if (!fp) {
        free(tmp_entries);
        return -1;
    }

    for (int i = 0; i < index->count; i++) {
        char hex_str[HASH_HEX_SIZE + 1];
        hash_to_hex(&tmp_entries[i].hash, hex_str);
        if (fprintf(fp, "%o %s %" PRIu64 " %u %s\n",
                    tmp_entries[i].mode,
                    hex_str,
                    tmp_entries[i].mtime_sec,
                    tmp_entries[i].size,
                    tmp_entries[i].path) < 0) {
            free(tmp_entries);
            fclose(fp);
            unlink(tmp_idx_path);
            return -1;
        }
    }

    fflush(fp);
    if (fsync(fileno(fp)) != 0) {
        free(tmp_entries);
        fclose(fp);
        unlink(tmp_idx_path);
        return -1;
    }

    if (fclose(fp) != 0) {
        free(tmp_entries);
        unlink(tmp_idx_path);
        return -1;
    }

    if (rename(tmp_idx_path, INDEX_FILE) != 0) {
        free(tmp_entries);
        unlink(tmp_idx_path);
        return -1;
    }

    free(tmp_entries);
    return 0;
}

// Stage a file for the next commit.
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    if (!index || !path) return -1;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    if (st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) return -1;

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    size_t data_len = (size_t)st.st_size;
    size_t alloc_len = data_len > 0 ? data_len : 1;
    void *data = malloc(alloc_len);
    if (!data) { fclose(fp); return -1; }

    if (data_len > 0 && fread(data, 1, data_len, fp) != data_len) {
        free(data);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    ObjectID blob_id;
    if (object_write(OBJ_BLOB, data, data_len, &blob_id) != 0) {
        free(data);
        return -1;
    }
    free(data);

    IndexEntry *ie = index_find(index, path);
    if (!ie) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        ie = &index->entries[index->count++];
    }

    ie->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    ie->hash = blob_id;
    ie->mtime_sec = (uint64_t)st.st_mtime;
    ie->size = (uint32_t)st.st_size;
    snprintf(ie->path, sizeof(ie->path), "%s", path);

    return index_save(index);
}
