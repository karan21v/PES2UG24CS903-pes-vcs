#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++)
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i+1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged = 0;
    for (int i = 0; i < index->count; i++) {
        printf("    staged: %s\n", index->entries[i].path);
        staged++;
    }
    if (staged == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("    deleted: %s\n", index->entries[i].path);
            unstaged++;
        } else if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                   st.st_size  != (off_t)index->entries[i].size) {
            printf("    modified: %s\n", index->entries[i].path);
            unstaged++;
        }
    }
    if (unstaged == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".")    == 0) continue;
            if (strcmp(ent->d_name, "..")   == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes")  == 0) continue;
            if (strstr(ent->d_name, ".o")  != NULL) continue;
            int tracked = 0;
            for (int i = 0; i < index->count; i++)
                if (strcmp(index->entries[i].path, ent->d_name) == 0)
                    { tracked = 1; break; }
            if (!tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("    untracked: %s\n", ent->d_name);
                    untracked++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked == 0) printf("    (nothing to show)\n");
    printf("\n");
    return 0;
}

static int compare_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry*)a)->path, ((const IndexEntry*)b)->path);
}

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof(line), f) && index->count < MAX_INDEX_ENTRIES) {
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') continue;
        IndexEntry *e = &index->entries[index->count];
        char hex[HASH_HEX_SIZE + 2];
        if (sscanf(line, "%o %64s %llu %llu %255s",
                   &e->mode, hex,
                   (unsigned long long*)&e->mtime_sec,
                   (unsigned long long*)&e->size,
                   e->path) != 5) continue;
        if (hex_to_hash(hex, &e->hash) != 0) continue;
        index->count++;
    }
    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    Index sorted = *index;
    qsort(sorted.entries, sorted.count, sizeof(IndexEntry), compare_entries);
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", INDEX_FILE);
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted.count; i++) {
        IndexEntry *e = &sorted.entries[i];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %llu %s\n",
                e->mode, hex,
                (unsigned long long)e->mtime_sec,
                (unsigned long long)e->size,
                e->path);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "error: cannot open '%s'\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(size);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, size, f) != size) { free(buf); fclose(f); return -1; }
    fclose(f);

    ObjectID id;
    if (object_write(OBJ_BLOB, buf, size, &id) != 0) { free(buf); return -1; }
    free(buf);

    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    uint32_t mode = S_ISDIR(st.st_mode) ? 0040000 :
                    (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    IndexEntry *e = index_find(index, path);
    if (!e) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        e = &index->entries[index->count++];
    }
    e->mode      = mode;
    e->hash      = id;
    e->mtime_sec = (uint64_t)st.st_mtime;
    e->size      = (uint64_t)st.st_size;
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = '\0';
    return index_save(index);
}
