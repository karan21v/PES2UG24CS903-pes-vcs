#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

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
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;
    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);
        ptr = space + 1;
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;
        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;
        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);
    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }
    *data_out = buffer;
    *len_out = offset;
    return 0;
}

static int write_tree_level(IndexEntry *entries, int count,
                             int prefix_len, ObjectID *id_out) {
    Tree tree;
    tree.count = 0;
    int i = 0;
    while (i < count) {
        const char *rel_path = entries[i].path + prefix_len;
        char *slash = strchr(rel_path, '/');
        if (!slash) {
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = entries[i].mode;
            strncpy(e->name, rel_path, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            e->hash = entries[i].hash;
            i++;
        } else {
            int dir_len = slash - rel_path;
            char dir_name[256];
            strncpy(dir_name, rel_path, dir_len);
            dir_name[dir_len] = '\0';
            int j = i;
            int new_prefix = prefix_len + dir_len + 1;
            while (j < count) {
                const char *rp = entries[j].path + prefix_len;
                if (strncmp(rp, dir_name, dir_len) == 0 && rp[dir_len] == '/')
                    j++;
                else
                    break;
            }
            ObjectID sub_id;
            if (write_tree_level(entries + i, j - i, new_prefix, &sub_id) != 0)
                return -1;
            TreeEntry *e = &tree.entries[tree.count++];
            e->mode = 0040000;
            strncpy(e->name, dir_name, sizeof(e->name) - 1);
            e->name[sizeof(e->name) - 1] = '\0';
            e->hash = sub_id;
            i = j;
        }
    }
    void *tree_data;
    size_t tree_len;
    if (tree_serialize(&tree, &tree_data, &tree_len) != 0) return -1;
    int rc = object_write(OBJ_TREE, tree_data, tree_len, id_out);
    free(tree_data);
    return rc;
}

int tree_from_index(ObjectID *id_out) {
    Index *index = malloc(sizeof(Index));
    if (!index) return -1;
    memset(index, 0, sizeof(Index));

    FILE *f = fopen(INDEX_FILE, "r");
    if (f) {
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
    }

    int rc = write_tree_level(index->entries, index->count, 0, id_out);
    free(index);
    return rc;
}
