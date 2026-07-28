#ifndef RELEASE_INFO_H
#define RELEASE_INFO_H

#include <stdbool.h>

#define LATTEROS_RELEASE_NAME "LatterOS"
#define LATTEROS_RELEASE_VERSION "0.20.3-rc1"
#define LATTEROS_RELEASE_MILESTONE "20D"
#define LATTEROS_RELEASE_CHANNEL "rc"
#define LATTEROS_RELEASE_ARCHITECTURE "x86_64"
#define LATTEROS_RELEASE_BUILD "local"

void release_info_init(void);

const char *release_info_name(void);
const char *release_info_version(void);
const char *release_info_milestone(void);
const char *release_info_channel(void);
const char *release_info_architecture(void);
const char *release_info_build(void);

bool release_info_write_files(void);

#endif
