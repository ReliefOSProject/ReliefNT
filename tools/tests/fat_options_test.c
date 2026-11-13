/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/fat_options.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/** @brief Check option ordering, ID overflow and failure-atomic parser state.
 * @return Zero after all production-parser assertions succeed. Host only.
 */
int main(void)
{
    struct fat_mount_options options;
    assert(!fat_options_parse("uid=1000,gid=1001,fmask=0133,dmask=0022", &options));
    assert(options.enabled && options.uid == 1000 && options.gid == 1001);
    assert(options.fmask == 0133 && options.dmask == 0022);
    assert(!fat_options_parse("umask=077,fmask=033", &options));
    assert(options.fmask == 033 && options.dmask == 077);
    assert(!fat_options_parse("fmask=033,umask=077", &options));
    assert(options.fmask == 077 && options.dmask == 077);
    const char *invalid[] = {"uid=", "uid=-1", "uid=4294967295", "gid=4294967296", "umask=008",
        "fmask=1000", "dmask=", "uid=1,", ",uid=1", "uid=1,,gid=1", "uid=1x", "exec", "label=x"};
    struct fat_mount_options before = options;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(fat_options_parse(invalid[i], &options) == -22);
        assert(!memcmp(&before, &options, sizeof(options)));
    }
    assert(!fat_options_parse("uid=4294967294", &options) && options.uid == 4294967294u);
    assert(!fat_options_parse("", &options) && !options.enabled);
    assert(fat_options_parse("uid=0", NULL) == -22);
    puts("PASS Linux FAT uid/gid, octal masks, option ordering, malformed input and atomic failure");
}
