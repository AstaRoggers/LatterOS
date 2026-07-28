#include "package_manager.h"

#include "release_info.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PACKAGE_ARCHIVE_MAGIC "LPKGv1\0"
#define PACKAGE_ARCHIVE_HEADER_SIZE 64U
#define PACKAGE_ARCHIVE_VERSION 1U
#define PACKAGE_FILE_ENTRY_SIZE 16U
#define PACKAGE_MAX_FILES 32U
#define PACKAGE_MAX_MANIFEST_SIZE 1024U
#define PACKAGE_MAX_ARCHIVE_SIZE (8U * 1024U * 1024U)
#define PACKAGE_IO_BUFFER_SIZE 512U
#define PACKAGE_RELATIVE_PATH_MAX 239U
#define PACKAGE_PATH_CAPACITY 512U
#define PACKAGE_METADATA_CAPACITY 768U

#define PACKAGE_HOME "/home/user/Applications"
#define PACKAGE_STATE_ROOT "/home/user/.latteros"
#define PACKAGE_DATABASE PACKAGE_STATE_ROOT "/packages"

#define PACKAGE_FILE_EXECUTABLE 0x0001U

typedef struct __attribute__((packed))
{
    uint8_t magic[8];
    uint32_t header_size;
    uint32_t version;
    uint32_t manifest_size;
    uint32_t file_count;
    uint32_t table_size;
    uint32_t payload_size;
    uint32_t body_crc32;
    uint8_t reserved[28];
} package_archive_header_t;

typedef struct __attribute__((packed))
{
    uint16_t path_length;
    uint16_t flags;
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t data_crc32;
} package_file_entry_t;

typedef struct
{
    vfs_node_t *node;
    package_archive_header_t header;
    package_info_t information;
    size_t table_offset;
    size_t payload_offset;
} package_archive_t;

static bool initialized;
static uint8_t io_buffer[PACKAGE_IO_BUFFER_SIZE];

static void clear_bytes(void *pointer, size_t count)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0; index < count; index++)
    {
        bytes[index] = 0;
    }
}

static size_t text_length(const char *text)
{
    size_t length = 0;

    if (text == NULL)
    {
        return 0;
    }

    while (text[length] != '\0')
    {
        length++;
    }

    return length;
}

static bool text_equal(const char *first, const char *second)
{
    if (first == NULL || second == NULL)
    {
        return false;
    }

    size_t index = 0;

    while (first[index] != '\0' && second[index] != '\0')
    {
        if (first[index] != second[index])
        {
            return false;
        }

        index++;
    }

    return first[index] == second[index];
}

static bool text_ends_with(const char *text, const char *suffix)
{
    size_t text_size = text_length(text);
    size_t suffix_size = text_length(suffix);

    if (suffix_size > text_size)
    {
        return false;
    }

    return text_equal(
        text + text_size - suffix_size,
        suffix
    );
}

static void copy_text(
    char *destination,
    size_t capacity,
    const char *source
)
{
    if (destination == NULL || capacity == 0)
    {
        return;
    }

    size_t index = 0;

    if (source != NULL)
    {
        while (
            source[index] != '\0' &&
            index + 1U < capacity
        )
        {
            destination[index] = source[index];
            index++;
        }
    }

    destination[index] = '\0';
}

static bool append_text(
    char *destination,
    size_t capacity,
    const char *source
)
{
    size_t position = text_length(destination);
    size_t input = 0;

    if (source == NULL)
    {
        return true;
    }

    while (source[input] != '\0')
    {
        if (position + 1U >= capacity)
        {
            return false;
        }

        destination[position++] = source[input++];
    }

    destination[position] = '\0';
    return true;
}

static bool ensure_directory(const char *path)
{
    vfs_node_t *node = vfs_open(path);

    if (node != NULL)
    {
        return node->type == VFS_NODE_DIRECTORY;
    }

    return vfs_make_directory(path);
}

static bool ensure_directory_tree(const char *path)
{
    char partial[PACKAGE_PATH_CAPACITY];
    copy_text(partial, sizeof(partial), path);

    size_t length = text_length(partial);

    if (length == 0 || partial[0] != '/')
    {
        return false;
    }

    for (size_t index = 1; index <= length; index++)
    {
        if (partial[index] != '/' && partial[index] != '\0')
        {
            continue;
        }

        char saved = partial[index];
        partial[index] = '\0';

        if (partial[0] != '\0' && !ensure_directory(partial))
        {
            partial[index] = saved;
            return false;
        }

        partial[index] = saved;
    }

    return true;
}

static bool ensure_parent_directories(const char *path)
{
    char parent[PACKAGE_PATH_CAPACITY];
    copy_text(parent, sizeof(parent), path);

    size_t length = text_length(parent);

    while (length > 0 && parent[length - 1U] != '/')
    {
        length--;
    }

    if (length == 0)
    {
        return false;
    }

    parent[length - 1U] = '\0';
    return ensure_directory_tree(parent);
}

static bool read_exact(
    vfs_node_t *node,
    size_t offset,
    void *buffer,
    size_t count
)
{
    return
        node != NULL &&
        buffer != NULL &&
        vfs_read(node, offset, buffer, count) == count;
}

static uint32_t crc32_update(
    uint32_t crc,
    const uint8_t *data,
    size_t count
)
{
    for (size_t index = 0; index < count; index++)
    {
        crc ^= data[index];

        for (uint32_t bit = 0; bit < 8U; bit++)
        {
            uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }

    return crc;
}

static bool calculate_node_crc(
    vfs_node_t *node,
    size_t offset,
    size_t count,
    uint32_t *result
)
{
    if (node == NULL || result == NULL)
    {
        return false;
    }

    uint32_t crc = 0xFFFFFFFFU;
    size_t remaining = count;

    while (remaining > 0)
    {
        size_t chunk = remaining > sizeof(io_buffer) ?
            sizeof(io_buffer) : remaining;

        if (!read_exact(node, offset, io_buffer, chunk))
        {
            return false;
        }

        crc = crc32_update(crc, io_buffer, chunk);
        offset += chunk;
        remaining -= chunk;
    }

    *result = crc ^ 0xFFFFFFFFU;
    return true;
}

static bool magic_valid(const uint8_t magic[8])
{
    static const uint8_t expected[8] = {
        'L', 'P', 'K', 'G', 'v', '1', '\0', '\0'
    };

    for (uint32_t index = 0; index < 8U; index++)
    {
        if (magic[index] != expected[index])
        {
            return false;
        }
    }

    return true;
}

static bool name_valid(const char *name)
{
    size_t length = text_length(name);

    if (length == 0 || length > PACKAGE_NAME_MAX || name[0] == '.')
    {
        return false;
    }

    for (size_t index = 0; index < length; index++)
    {
        char character = name[index];
        bool allowed =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '-' || character == '_' || character == '.';

        if (!allowed)
        {
            return false;
        }
    }

    return true;
}

static bool relative_path_valid(const char *path)
{
    size_t length = text_length(path);

    if (
        length == 0 ||
        length > PACKAGE_RELATIVE_PATH_MAX ||
        path[0] == '/' ||
        path[length - 1U] == '/'
    )
    {
        return false;
    }

    size_t component_start = 0;

    for (size_t index = 0; index <= length; index++)
    {
        char character = path[index];

        if (
            character == '\\' ||
            (character != '\0' && (uint8_t)character < 32U)
        )
        {
            return false;
        }

        if (character != '/' && character != '\0')
        {
            continue;
        }

        size_t component_length = index - component_start;

        if (
            component_length == 0 ||
            (component_length == 1U &&
             path[component_start] == '.') ||
            (component_length == 2U &&
             path[component_start] == '.' &&
             path[component_start + 1U] == '.')
        )
        {
            return false;
        }

        component_start = index + 1U;
    }

    return true;
}

static void trim_text(char *text)
{
    size_t length = text_length(text);
    size_t start = 0;

    while (text[start] == ' ' || text[start] == '\t')
    {
        start++;
    }

    while (
        length > start &&
        (text[length - 1U] == ' ' ||
         text[length - 1U] == '\t' ||
         text[length - 1U] == '\r' ||
         text[length - 1U] == '\n')
    )
    {
        length--;
    }

    if (start > 0)
    {
        size_t output = 0;

        while (start < length)
        {
            text[output++] = text[start++];
        }

        text[output] = '\0';
    }
    else
    {
        text[length] = '\0';
    }
}

static bool manifest_value(
    const char *manifest,
    const char *key,
    char *output,
    size_t capacity
)
{
    size_t key_length = text_length(key);
    size_t position = 0;

    while (manifest[position] != '\0')
    {
        size_t line_start = position;

        while (
            manifest[position] != '\0' &&
            manifest[position] != '\n'
        )
        {
            position++;
        }

        size_t line_length = position - line_start;

        if (
            line_length > key_length + 1U &&
            manifest[line_start + key_length] == '='
        )
        {
            bool matches = true;

            for (size_t index = 0; index < key_length; index++)
            {
                if (manifest[line_start + index] != key[index])
                {
                    matches = false;
                    break;
                }
            }

            if (matches)
            {
                size_t value_start = line_start + key_length + 1U;
                size_t value_length = line_start + line_length - value_start;

                if (value_length + 1U > capacity)
                {
                    return false;
                }

                for (size_t index = 0; index < value_length; index++)
                {
                    output[index] = manifest[value_start + index];
                }

                output[value_length] = '\0';
                trim_text(output);
                return true;
            }
        }

        if (manifest[position] == '\n')
        {
            position++;
        }
    }

    output[0] = '\0';
    return false;
}

static bool parse_manifest(
    const char *manifest,
    package_info_t *information
)
{
    clear_bytes(information, sizeof(*information));

    if (
        !manifest_value(
            manifest,
            "name",
            information->name,
            sizeof(information->name)
        ) ||
        !manifest_value(
            manifest,
            "version",
            information->version,
            sizeof(information->version)
        )
    )
    {
        return false;
    }

    (void)manifest_value(
        manifest,
        "description",
        information->description,
        sizeof(information->description)
    );

    (void)manifest_value(
        manifest,
        "depends",
        information->depends,
        sizeof(information->depends)
    );

    (void)manifest_value(
        manifest,
        "entry",
        information->entry,
        sizeof(information->entry)
    );

    return
        name_valid(information->name) &&
        information->version[0] != '\0';
}

static package_result_t archive_open(
    const char *path,
    package_archive_t *archive
)
{
    if (path == NULL || archive == NULL)
    {
        return PACKAGE_RESULT_INVALID_ARGUMENT;
    }

    clear_bytes(archive, sizeof(*archive));
    archive->node = vfs_open(path);

    if (
        archive->node == NULL ||
        archive->node->type != VFS_NODE_FILE
    )
    {
        return PACKAGE_RESULT_NOT_FOUND;
    }

    if (
        archive->node->size < PACKAGE_ARCHIVE_HEADER_SIZE ||
        archive->node->size > PACKAGE_MAX_ARCHIVE_SIZE ||
        !read_exact(
            archive->node,
            0,
            &archive->header,
            sizeof(archive->header)
        )
    )
    {
        return PACKAGE_RESULT_BAD_ARCHIVE;
    }

    if (!magic_valid(archive->header.magic))
    {
        return PACKAGE_RESULT_BAD_ARCHIVE;
    }

    if (
        archive->header.version != PACKAGE_ARCHIVE_VERSION ||
        archive->header.header_size != PACKAGE_ARCHIVE_HEADER_SIZE
    )
    {
        return PACKAGE_RESULT_UNSUPPORTED_VERSION;
    }

    if (
        archive->header.manifest_size == 0 ||
        archive->header.manifest_size > PACKAGE_MAX_MANIFEST_SIZE ||
        archive->header.file_count == 0 ||
        archive->header.file_count > PACKAGE_MAX_FILES ||
        archive->header.table_size <
            archive->header.file_count * PACKAGE_FILE_ENTRY_SIZE
    )
    {
        return PACKAGE_RESULT_BAD_ARCHIVE;
    }

    size_t expected_size =
        (size_t)archive->header.header_size +
        archive->header.manifest_size +
        archive->header.table_size +
        archive->header.payload_size;

    if (expected_size != archive->node->size)
    {
        return PACKAGE_RESULT_BAD_ARCHIVE;
    }

    uint32_t body_crc = 0;

    if (
        !calculate_node_crc(
            archive->node,
            archive->header.header_size,
            archive->node->size - archive->header.header_size,
            &body_crc
        ) ||
        body_crc != archive->header.body_crc32
    )
    {
        return PACKAGE_RESULT_CHECKSUM_FAILED;
    }

    char manifest[PACKAGE_MAX_MANIFEST_SIZE + 1U];

    if (!read_exact(
        archive->node,
        archive->header.header_size,
        manifest,
        archive->header.manifest_size
    ))
    {
        return PACKAGE_RESULT_BAD_ARCHIVE;
    }

    manifest[archive->header.manifest_size] = '\0';

    if (!parse_manifest(manifest, &archive->information))
    {
        return PACKAGE_RESULT_INVALID_MANIFEST;
    }

    archive->information.file_count = archive->header.file_count;
    archive->information.payload_size = archive->header.payload_size;
    archive->information.installed =
        package_manager_is_installed(archive->information.name);
    archive->table_offset =
        archive->header.header_size + archive->header.manifest_size;
    archive->payload_offset =
        archive->table_offset + archive->header.table_size;

    return PACKAGE_RESULT_OK;
}

static bool build_named_path(
    char *output,
    size_t capacity,
    const char *prefix,
    const char *name,
    const char *suffix
)
{
    output[0] = '\0';

    return
        append_text(output, capacity, prefix) &&
        append_text(output, capacity, name) &&
        append_text(output, capacity, suffix);
}

static bool metadata_path(
    const char *name,
    char output[PACKAGE_PATH_CAPACITY]
)
{
    return build_named_path(
        output,
        PACKAGE_PATH_CAPACITY,
        PACKAGE_DATABASE "/",
        name,
        ".meta"
    );
}

static bool application_path(
    const char *name,
    char output[PACKAGE_PATH_CAPACITY]
)
{
    return build_named_path(
        output,
        PACKAGE_PATH_CAPACITY,
        PACKAGE_HOME "/",
        name,
        ""
    );
}

static bool read_text_file(
    const char *path,
    char *buffer,
    size_t capacity
)
{
    vfs_node_t *node = vfs_open(path);

    if (
        node == NULL ||
        node->type != VFS_NODE_FILE ||
        node->size + 1U > capacity
    )
    {
        return false;
    }

    size_t count = vfs_read(node, 0, buffer, node->size);

    if (count != node->size)
    {
        return false;
    }

    buffer[count] = '\0';
    return true;
}

static bool read_installed_metadata(
    const char *name,
    package_info_t *information
)
{
    char path[PACKAGE_PATH_CAPACITY];
    char metadata[PACKAGE_METADATA_CAPACITY];

    if (
        !name_valid(name) ||
        !metadata_path(name, path) ||
        !read_text_file(path, metadata, sizeof(metadata)) ||
        !parse_manifest(metadata, information)
    )
    {
        return false;
    }

    char files[16];
    char payload[16];

    if (manifest_value(metadata, "files", files, sizeof(files)))
    {
        uint32_t value = 0;

        for (size_t index = 0; files[index] >= '0' && files[index] <= '9'; index++)
        {
            value = value * 10U + (uint32_t)(files[index] - '0');
        }

        information->file_count = value;
    }

    if (manifest_value(metadata, "payload", payload, sizeof(payload)))
    {
        uint32_t value = 0;

        for (size_t index = 0; payload[index] >= '0' && payload[index] <= '9'; index++)
        {
            value = value * 10U + (uint32_t)(payload[index] - '0');
        }

        information->payload_size = value;
    }

    information->installed = true;
    return true;
}

static void append_uint32(
    char *text,
    size_t capacity,
    uint32_t value
)
{
    char reverse[11];
    uint32_t count = 0;

    if (value == 0)
    {
        (void)append_text(text, capacity, "0");
        return;
    }

    while (value > 0 && count < sizeof(reverse))
    {
        reverse[count++] = (char)('0' + value % 10U);
        value /= 10U;
    }

    while (count > 0)
    {
        char character[2] = { reverse[--count], '\0' };
        (void)append_text(text, capacity, character);
    }
}

static bool write_installed_metadata(
    const package_info_t *information
)
{
    char path[PACKAGE_PATH_CAPACITY];
    char metadata[PACKAGE_METADATA_CAPACITY];

    if (
        information == NULL ||
        !metadata_path(information->name, path)
    )
    {
        return false;
    }

    metadata[0] = '\0';

    bool success =
        append_text(metadata, sizeof(metadata), "name=") &&
        append_text(metadata, sizeof(metadata), information->name) &&
        append_text(metadata, sizeof(metadata), "\nversion=") &&
        append_text(metadata, sizeof(metadata), information->version) &&
        append_text(metadata, sizeof(metadata), "\ndescription=") &&
        append_text(metadata, sizeof(metadata), information->description) &&
        append_text(metadata, sizeof(metadata), "\ndepends=") &&
        append_text(metadata, sizeof(metadata), information->depends) &&
        append_text(metadata, sizeof(metadata), "\nentry=") &&
        append_text(metadata, sizeof(metadata), information->entry) &&
        append_text(metadata, sizeof(metadata), "\nfiles=");

    if (!success)
    {
        return false;
    }

    append_uint32(metadata, sizeof(metadata), information->file_count);
    success = append_text(metadata, sizeof(metadata), "\npayload=");
    append_uint32(metadata, sizeof(metadata), information->payload_size);
    success = success && append_text(metadata, sizeof(metadata), "\n");

    return success && vfs_write_text(path, metadata);
}

static bool dependency_list_contains(
    const char *dependencies,
    const char *name
)
{
    size_t position = 0;

    while (dependencies != NULL && dependencies[position] != '\0')
    {
        while (
            dependencies[position] == ' ' ||
            dependencies[position] == '\t' ||
            dependencies[position] == ','
        )
        {
            position++;
        }

        char dependency[PACKAGE_NAME_MAX + 1U];
        size_t output = 0;

        while (
            dependencies[position] != '\0' &&
            dependencies[position] != ','
        )
        {
            if (output < PACKAGE_NAME_MAX)
            {
                dependency[output++] = dependencies[position];
            }

            position++;
        }

        dependency[output] = '\0';
        trim_text(dependency);

        if (dependency[0] != '\0' && text_equal(dependency, name))
        {
            return true;
        }
    }

    return false;
}

static bool dependencies_available(const char *dependencies)
{
    size_t position = 0;

    while (dependencies != NULL && dependencies[position] != '\0')
    {
        while (
            dependencies[position] == ' ' ||
            dependencies[position] == '\t' ||
            dependencies[position] == ','
        )
        {
            position++;
        }

        char dependency[PACKAGE_NAME_MAX + 1U];
        size_t output = 0;

        while (
            dependencies[position] != '\0' &&
            dependencies[position] != ','
        )
        {
            if (output < PACKAGE_NAME_MAX)
            {
                dependency[output++] = dependencies[position];
            }

            position++;
        }

        dependency[output] = '\0';
        trim_text(dependency);

        if (
            dependency[0] != '\0' &&
            !package_manager_is_installed(dependency)
        )
        {
            return false;
        }
    }

    return true;
}

static uint32_t version_component(
    const char *version,
    size_t *position
)
{
    uint32_t value = 0;

    while (
        version[*position] >= '0' &&
        version[*position] <= '9'
    )
    {
        value = value * 10U +
            (uint32_t)(version[*position] - '0');
        (*position)++;
    }

    while (
        version[*position] != '\0' &&
        version[*position] != '.'
    )
    {
        (*position)++;
    }

    if (version[*position] == '.')
    {
        (*position)++;
    }

    return value;
}

static int compare_versions(const char *first, const char *second)
{
    size_t first_position = 0;
    size_t second_position = 0;

    for (uint32_t component = 0; component < 4U; component++)
    {
        uint32_t first_value = version_component(first, &first_position);
        uint32_t second_value = version_component(second, &second_position);

        if (first_value < second_value)
        {
            return -1;
        }

        if (first_value > second_value)
        {
            return 1;
        }
    }

    return 0;
}

static package_result_t install_archive_files(
    const package_archive_t *archive,
    const char *staging_root
)
{
    size_t table_cursor = archive->table_offset;
    size_t table_end = archive->table_offset + archive->header.table_size;

    for (uint32_t index = 0; index < archive->header.file_count; index++)
    {
        package_file_entry_t entry;

        if (
            table_cursor + sizeof(entry) > table_end ||
            !read_exact(
                archive->node,
                table_cursor,
                &entry,
                sizeof(entry)
            )
        )
        {
            return PACKAGE_RESULT_BAD_ARCHIVE;
        }

        table_cursor += sizeof(entry);

        if (
            entry.path_length == 0 ||
            entry.path_length > PACKAGE_RELATIVE_PATH_MAX ||
            table_cursor + entry.path_length > table_end ||
            entry.data_offset > archive->header.payload_size ||
            entry.data_size >
                archive->header.payload_size - entry.data_offset
        )
        {
            return PACKAGE_RESULT_BAD_ARCHIVE;
        }

        char relative[PACKAGE_RELATIVE_PATH_MAX + 1U];

        if (!read_exact(
            archive->node,
            table_cursor,
            relative,
            entry.path_length
        ))
        {
            return PACKAGE_RESULT_BAD_ARCHIVE;
        }

        relative[entry.path_length] = '\0';
        table_cursor += entry.path_length;

        if (!relative_path_valid(relative))
        {
            return PACKAGE_RESULT_INVALID_PATH;
        }

        char destination[PACKAGE_PATH_CAPACITY];
        destination[0] = '\0';

        if (
            !append_text(destination, sizeof(destination), staging_root) ||
            !append_text(destination, sizeof(destination), "/") ||
            !append_text(destination, sizeof(destination), relative) ||
            !ensure_parent_directories(destination)
        )
        {
            return PACKAGE_RESULT_FILESYSTEM_ERROR;
        }

        if (!vfs_create_file(destination))
        {
            vfs_node_t *existing = vfs_open(destination);

            if (existing == NULL || existing->type != VFS_NODE_FILE)
            {
                return PACKAGE_RESULT_FILESYSTEM_ERROR;
            }
        }

        vfs_node_t *output = vfs_open(destination);

        if (output == NULL || !vfs_truncate(output))
        {
            return PACKAGE_RESULT_FILESYSTEM_ERROR;
        }

        size_t source_offset =
            archive->payload_offset + entry.data_offset;
        size_t written = 0;
        uint32_t crc = 0xFFFFFFFFU;

        while (written < entry.data_size)
        {
            size_t remaining = entry.data_size - written;
            size_t chunk = remaining > sizeof(io_buffer) ?
                sizeof(io_buffer) : remaining;

            if (
                !read_exact(
                    archive->node,
                    source_offset + written,
                    io_buffer,
                    chunk
                ) ||
                vfs_write(output, written, io_buffer, chunk) != chunk
            )
            {
                return PACKAGE_RESULT_FILESYSTEM_ERROR;
            }

            crc = crc32_update(crc, io_buffer, chunk);
            written += chunk;
        }

        crc ^= 0xFFFFFFFFU;

        if (crc != entry.data_crc32)
        {
            return PACKAGE_RESULT_CHECKSUM_FAILED;
        }

        (void)vfs_chmod(
            destination,
            (entry.flags & PACKAGE_FILE_EXECUTABLE) != 0 ?
                VFS_MODE_EXECUTABLE_DEFAULT :
                VFS_MODE_FILE_DEFAULT
        );
    }

    return table_cursor == table_end ?
        PACKAGE_RESULT_OK : PACKAGE_RESULT_BAD_ARCHIVE;
}

void package_manager_init(void)
{
    if (initialized)
    {
        return;
    }

    initialized = true;
    release_info_init();

    (void)ensure_directory_tree(PACKAGE_HOME);
    (void)ensure_directory_tree(PACKAGE_DATABASE);
}

bool package_manager_is_installed(const char *name)
{
    package_manager_init();

    char path[PACKAGE_PATH_CAPACITY];

    return
        name_valid(name) &&
        metadata_path(name, path) &&
        vfs_open(path) != NULL;
}

package_result_t package_manager_probe(
    const char *archive_path,
    package_info_t *information
)
{
    package_manager_init();

    package_archive_t archive;
    package_result_t result = archive_open(archive_path, &archive);

    if (result == PACKAGE_RESULT_OK && information != NULL)
    {
        *information = archive.information;
    }

    return result;
}

package_result_t package_manager_install(
    const char *archive_path,
    package_info_t *installed_information
)
{
    package_manager_init();

    package_archive_t archive;
    package_result_t result = archive_open(archive_path, &archive);

    if (result != PACKAGE_RESULT_OK)
    {
        return result;
    }

    if (!dependencies_available(archive.information.depends))
    {
        return PACKAGE_RESULT_DEPENDENCY_MISSING;
    }

    package_info_t previous_information;
    bool previously_installed = read_installed_metadata(
        archive.information.name,
        &previous_information
    );

    if (
        previously_installed &&
        compare_versions(
            archive.information.version,
            previous_information.version
        ) < 0
    )
    {
        return PACKAGE_RESULT_DOWNGRADE_BLOCKED;
    }

    char final_root[PACKAGE_PATH_CAPACITY];
    char staging_root[PACKAGE_PATH_CAPACITY];
    char backup_root[PACKAGE_PATH_CAPACITY];

    if (
        !application_path(archive.information.name, final_root) ||
        !build_named_path(
            staging_root,
            sizeof(staging_root),
            PACKAGE_HOME "/.install-",
            archive.information.name,
            ""
        ) ||
        !build_named_path(
            backup_root,
            sizeof(backup_root),
            PACKAGE_HOME "/.backup-",
            archive.information.name,
            ""
        )
    )
    {
        return PACKAGE_RESULT_INVALID_NAME;
    }

    (void)vfs_remove(staging_root, true);
    (void)vfs_remove(backup_root, true);

    if (!ensure_directory(staging_root))
    {
        return PACKAGE_RESULT_FILESYSTEM_ERROR;
    }

    result = install_archive_files(&archive, staging_root);

    if (result != PACKAGE_RESULT_OK)
    {
        (void)vfs_remove(staging_root, true);
        return result;
    }

    bool backup_created = false;

    if (vfs_open(final_root) != NULL)
    {
        if (!vfs_move(final_root, backup_root))
        {
            (void)vfs_remove(staging_root, true);
            return PACKAGE_RESULT_FILESYSTEM_ERROR;
        }

        backup_created = true;
    }

    if (!vfs_move(staging_root, final_root))
    {
        if (backup_created)
        {
            (void)vfs_move(backup_root, final_root);
        }

        (void)vfs_remove(staging_root, true);
        return PACKAGE_RESULT_FILESYSTEM_ERROR;
    }

    archive.information.installed = true;

    if (!write_installed_metadata(&archive.information))
    {
        (void)vfs_remove(final_root, true);

        if (backup_created)
        {
            (void)vfs_move(backup_root, final_root);
            (void)write_installed_metadata(&previous_information);
        }

        return PACKAGE_RESULT_FILESYSTEM_ERROR;
    }

    if (backup_created)
    {
        (void)vfs_remove(backup_root, true);
    }

    if (installed_information != NULL)
    {
        *installed_information = archive.information;
    }

    return PACKAGE_RESULT_OK;
}

static bool package_is_required(const char *name)
{
    vfs_node_t *database = vfs_open(PACKAGE_DATABASE);

    if (database == NULL)
    {
        return false;
    }

    for (
        vfs_node_t *node = database->first_child;
        node != NULL;
        node = node->next_sibling
    )
    {
        if (
            node->type != VFS_NODE_FILE ||
            !text_ends_with(node->name, ".meta")
        )
        {
            continue;
        }

        char installed_name[PACKAGE_NAME_MAX + 1U];
        size_t name_length = text_length(node->name) - 5U;

        if (name_length > PACKAGE_NAME_MAX)
        {
            continue;
        }

        for (size_t index = 0; index < name_length; index++)
        {
            installed_name[index] = node->name[index];
        }

        installed_name[name_length] = '\0';

        if (text_equal(installed_name, name))
        {
            continue;
        }

        package_info_t information;

        if (
            read_installed_metadata(installed_name, &information) &&
            dependency_list_contains(information.depends, name)
        )
        {
            return true;
        }
    }

    return false;
}

package_result_t package_manager_remove(const char *name)
{
    package_manager_init();

    if (!name_valid(name))
    {
        return PACKAGE_RESULT_INVALID_NAME;
    }

    if (!package_manager_is_installed(name))
    {
        return PACKAGE_RESULT_NOT_INSTALLED;
    }

    if (package_is_required(name))
    {
        return PACKAGE_RESULT_DEPENDENCY_IN_USE;
    }

    char root[PACKAGE_PATH_CAPACITY];
    char metadata[PACKAGE_PATH_CAPACITY];

    if (
        !application_path(name, root) ||
        !metadata_path(name, metadata)
    )
    {
        return PACKAGE_RESULT_INVALID_NAME;
    }

    if (
        vfs_open(root) != NULL &&
        !vfs_remove(root, true)
    )
    {
        return PACKAGE_RESULT_FILESYSTEM_ERROR;
    }

    if (!vfs_remove(metadata, false))
    {
        return PACKAGE_RESULT_FILESYSTEM_ERROR;
    }

    return PACKAGE_RESULT_OK;
}

uint32_t package_manager_installed_count(void)
{
    package_manager_init();

    vfs_node_t *database = vfs_open(PACKAGE_DATABASE);

    if (database == NULL)
    {
        return 0;
    }

    uint32_t count = 0;

    for (
        vfs_node_t *node = database->first_child;
        node != NULL;
        node = node->next_sibling
    )
    {
        if (
            node->type == VFS_NODE_FILE &&
            text_ends_with(node->name, ".meta")
        )
        {
            count++;
        }
    }

    return count;
}

bool package_manager_installed_get(
    uint32_t index,
    package_info_t *information
)
{
    package_manager_init();

    if (information == NULL)
    {
        return false;
    }

    vfs_node_t *database = vfs_open(PACKAGE_DATABASE);

    if (database == NULL)
    {
        return false;
    }

    uint32_t current = 0;

    for (
        vfs_node_t *node = database->first_child;
        node != NULL;
        node = node->next_sibling
    )
    {
        if (
            node->type != VFS_NODE_FILE ||
            !text_ends_with(node->name, ".meta")
        )
        {
            continue;
        }

        if (current++ != index)
        {
            continue;
        }

        size_t name_length = text_length(node->name) - 5U;

        if (name_length > PACKAGE_NAME_MAX)
        {
            return false;
        }

        char name[PACKAGE_NAME_MAX + 1U];

        for (size_t offset = 0; offset < name_length; offset++)
        {
            name[offset] = node->name[offset];
        }

        name[name_length] = '\0';
        return read_installed_metadata(name, information);
    }

    return false;
}

const char *package_result_message(package_result_t result)
{
    switch (result)
    {
        case PACKAGE_RESULT_OK:
            return "Package operation completed";
        case PACKAGE_RESULT_INVALID_ARGUMENT:
            return "Invalid package argument";
        case PACKAGE_RESULT_NOT_FOUND:
            return "Package archive not found";
        case PACKAGE_RESULT_BAD_ARCHIVE:
            return "Malformed LPKG archive";
        case PACKAGE_RESULT_UNSUPPORTED_VERSION:
            return "Unsupported LPKG version";
        case PACKAGE_RESULT_CHECKSUM_FAILED:
            return "Package checksum verification failed";
        case PACKAGE_RESULT_INVALID_MANIFEST:
            return "Package manifest is invalid";
        case PACKAGE_RESULT_INVALID_NAME:
            return "Package name is invalid";
        case PACKAGE_RESULT_INVALID_PATH:
            return "Package contains an unsafe path";
        case PACKAGE_RESULT_DEPENDENCY_MISSING:
            return "A package dependency is missing";
        case PACKAGE_RESULT_DEPENDENCY_IN_USE:
            return "Another package depends on this package";
        case PACKAGE_RESULT_DOWNGRADE_BLOCKED:
            return "Package downgrade blocked";
        case PACKAGE_RESULT_FILESYSTEM_ERROR:
            return "Package filesystem operation failed";
        case PACKAGE_RESULT_NOT_INSTALLED:
            return "Package is not installed";
        default:
            return "Unknown package error";
    }
}
