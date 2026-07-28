#include "release_info.h"

#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>

#define RELEASE_DIRECTORY "/home/user/.latteros"
#define RELEASE_METADATA_PATH RELEASE_DIRECTORY "/release.meta"
#define RELEASE_NOTES_PATH "/home/user/Documents/LatterOS Release.txt"

static bool initialized;

static bool ensure_directory(const char *path)
{
    vfs_node_t *node = vfs_open(path);

    if (node != NULL)
    {
        return node->type == VFS_NODE_DIRECTORY;
    }

    return vfs_make_directory(path);
}

const char *release_info_name(void)
{
    return LATTEROS_RELEASE_NAME;
}

const char *release_info_version(void)
{
    return LATTEROS_RELEASE_VERSION;
}

const char *release_info_milestone(void)
{
    return LATTEROS_RELEASE_MILESTONE;
}

const char *release_info_channel(void)
{
    return LATTEROS_RELEASE_CHANNEL;
}

const char *release_info_architecture(void)
{
    return LATTEROS_RELEASE_ARCHITECTURE;
}

const char *release_info_build(void)
{
    return LATTEROS_RELEASE_BUILD;
}

bool release_info_write_files(void)
{
    if (
        !ensure_directory("/home") ||
        !ensure_directory("/home/user") ||
        !ensure_directory("/home/user/Documents") ||
        !ensure_directory(RELEASE_DIRECTORY)
    )
    {
        return false;
    }

    static const char metadata[] =
        "name=" LATTEROS_RELEASE_NAME "\n"
        "version=" LATTEROS_RELEASE_VERSION "\n"
        "milestone=" LATTEROS_RELEASE_MILESTONE "\n"
        "channel=" LATTEROS_RELEASE_CHANNEL "\n"
        "architecture=" LATTEROS_RELEASE_ARCHITECTURE "\n"
        "build=" LATTEROS_RELEASE_BUILD "\n";

    static const char notes[] =
        "LatterOS " LATTEROS_RELEASE_VERSION "\n"
        "Milestone " LATTEROS_RELEASE_MILESTONE "\n"
        "\n"
        "This release begins hardware compatibility and stabilization work.\n"
        "It adds a non-destructive compatibility dashboard, CPU feature\n"
        "inventory, ACPI/PCI/storage/USB/network reporting, a saved hardware\n"
        "report, and a conservative Hardware Test boot entry.\n"
        "\n"
        "Open Settings > Hardware to refresh the profile, run the quick\n"
        "compatibility test, and save the full report in Documents.\n";

    return
        vfs_write_text(RELEASE_METADATA_PATH, metadata) &&
        vfs_write_text(RELEASE_NOTES_PATH, notes);
}

void release_info_init(void)
{
    if (initialized)
    {
        return;
    }

    initialized = true;
    (void)release_info_write_files();
}
