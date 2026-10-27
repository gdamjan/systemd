/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sd-bus.h"

#include "alloc-util.h"
#include "dirent-util.h"
#include "discover-image.h"
#include "errno-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "hashmap.h"
#include "image-clean-pool.h"
#include "io-util.h"
#include "log.h"
#include "set.h"
#include "stat-util.h"
#include "string-table.h"

static const char* const image_clean_pool_mode_table[_IMAGE_CLEAN_POOL_MAX] = {
        [IMAGE_CLEAN_POOL_REMOVE_ALL]    = "all",
        [IMAGE_CLEAN_POOL_REMOVE_HIDDEN] = "hidden"
};

DEFINE_STRING_TABLE_LOOKUP(image_clean_pool_mode, ImageCleanPoolMode);

int image_collect_mstack_layers(Hashmap *images, Set **ret) {
        _cleanup_set_free_ Set *inodes = NULL;
        Image *image;
        int r;

        assert(ret);

        /* Collects the inodes of everything that is symlinked from a .mstack/ directory, so that we don't
         * remove hidden images that are still referenced as layers of an existing mstack. */

        HASHMAP_FOREACH(image, images) {
                if (image->type != IMAGE_MSTACK)
                        continue;

                _cleanup_closedir_ DIR *d = opendir(image->path);
                if (!d) {
                        log_debug_errno(errno, "Failed to open mstack directory '%s', ignoring: %m", image->path);
                        continue;
                }

                FOREACH_DIRENT(de, d, return log_debug_errno(errno, "Failed to read mstack directory '%s': %m", image->path)) {
                        if (de->d_type != DT_LNK)
                                continue;

                        _cleanup_free_ struct stat *st = new(struct stat, 1);
                        if (!st)
                                return log_oom_debug();

                        if (fstatat(dirfd(d), de->d_name, st, 0) < 0) {
                                log_debug_errno(errno, "Failed to stat '%s/%s', ignoring: %m", image->path, de->d_name);
                                continue;
                        }

                        r = set_ensure_consume(&inodes, &inode_hash_ops, TAKE_PTR(st));
                        if (r < 0)
                                return log_oom_debug();
                }
        }

        *ret = TAKE_PTR(inodes);
        return 0;
}

bool image_is_mstack_layer(const Image *image, Set *mstack_layers) {
        struct stat st;

        assert(image);

        if (set_isempty(mstack_layers))
                return false;

        if (stat(image->path, &st) < 0) {
                log_debug_errno(errno, "Failed to stat '%s', ignoring: %m", image->path);
                return false;
        }

        return set_contains(mstack_layers, &st);
}

int image_clean_pool_remove(Image **images, size_t n_images, RuntimeScope scope, int result_fd) {
        int r;

        assert(images || n_images == 0);
        assert(result_fd >= 0);

        /* Removes the specified images, and writes the result into the specified file: a boolean indicating
         * success, followed by the name and exclusive disk usage of each removed image. On failure, the
         * contents are replaced by a boolean indicating failure, followed by the name of the image that
         * could not be removed. This is supposed to be run in a forked off child process, and the file
         * is then parsed with clean_pool_read_first_entry() and clean_pool_read_next_entry(). */

        bool success = true;
        r = loop_write(result_fd, &success, sizeof(success));
        if (r < 0)
                return log_debug_errno(r, "Failed to write to tmp file: %m");

        FOREACH_ARRAY(i, images, n_images) {
                Image *image = *i;

                r = image_remove(image, scope);
                if (r == -EBUSY) {
                        log_debug("Keeping image '%s' because it's currently used.", image->name);
                        continue;
                }
                if (r < 0) {
                        log_debug_errno(r, "Failed to remove image '%s': %m", image->name);
                        success = false;

                        /* If the operation failed, let's override everything we wrote, and instead write there at which image we failed. */
                        if (ftruncate(result_fd, 0) < 0 ||
                            lseek(result_fd, 0, SEEK_SET) < 0 ||
                            write(result_fd, &success, sizeof(success)) < 0 ||
                            write(result_fd, image->name, strlen(image->name)+1) < 0)
                                log_debug_errno(errno, "Failed to truncate, rewind, or write to tmp file, ignoring: %m");

                        /* Report original error code (not results of rewind/truncate/etc */
                        return r;
                }

                r = loop_write(result_fd, image->name, strlen(image->name) + 1);
                if (r < 0)
                        return log_debug_errno(r, "Failed to write image name to tmp file: %m");

                r = loop_write(result_fd, &image->usage_exclusive, sizeof(image->usage_exclusive));
                if (r < 0)
                        return log_debug_errno(r, "Failed to write image's usage to tmp file: %m");
        }

        return 0;
}

int clean_pool_read_first_entry(FILE *file, int child_error, sd_bus_error *error) {
        _cleanup_free_ char *name = NULL;
        bool success;
        int r;

        assert(file);

        if (fseek(file, 0, SEEK_SET) < 0)
                return log_debug_errno(errno, "Failed to seek to the beginning of tmp file: %m");

        /* The resulting temporary file starts with a boolean value that indicates success or not. */
        errno = 0;
        size_t n = fread(&success, 1, sizeof(success), file);
        if (n != sizeof(success)) {
                log_debug_errno(errno_or_else(EIO), "Received unexpected amount of %zu bytes: %m", n);
                return child_error < 0 ? child_error : errno_or_else(EIO);
        }

        if (child_error < 0) {
                /* The clean-up operation failed. In this case the resulting temporary file should contain a boolean
                 * set to false followed by the name of the failed image. Let's try to read this and use it for the
                 * error message. If we can't read it, don't mind, and return the naked error. */

                if (success) { /* The resulting temporary file could not be updated, ignore it. */
                        log_debug("Child process exited with an error. We expected a name of the image which caused the failure in the tmp file, but it failed as well.");
                        return child_error;
                }

                r = read_nul_string(file, LONG_LINE_MAX, &name);
                if (r <= 0) { /* Same here... */
                        log_debug_errno(r, "Failed to read nul-terminated string from tmp file: %m");
                        return child_error;
                }

                log_debug_errno(child_error, "Failed to remove image '%s': %m", name);
                return sd_bus_error_set_errnof(error, child_error, "Failed to remove image '%s': %m", name);
        }

        if (!success)
                return log_debug_errno(SYNTHETIC_ERRNO(ESTALE), "Inconsistent state of the temporary file.");

        return 0;
}

int clean_pool_read_next_entry(FILE *file, char **ret_name, uint64_t *ret_usage) {
        /* Return value:
         * r < 0: error
         * r == 0 last record returned
         * r > 0 more records expected */

        _cleanup_free_ char *name = NULL;
        uint64_t usage;
        int r;

        assert(file);

        r = read_nul_string(file, LONG_LINE_MAX, &name);
        if (r < 0)
                return log_debug_errno(r, "Failed to read nul-terminated string: %m");
        if (r == 0) /* reached the end */
                return 0;

        errno = 0;
        size_t n = fread(&usage, 1, sizeof(usage), file);
        if (n != sizeof(usage))
                return log_debug_errno(errno_or_else(EIO), "Received unexpected amount of %zu bytes: %m", n);

        if (ret_name)
                *ret_name = TAKE_PTR(name);
        if (ret_usage)
                *ret_usage = usage;

        return 1;
}

int image_clean_pool_reply(sd_bus_message *message, int *result_fd, int child_error, sd_bus_error *error) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_fclose_ FILE *file = NULL;
        int r;

        assert(message);
        assert(result_fd);
        assert(*result_fd >= 0);

        file = take_fdopen(result_fd, "r");
        if (!file)
                return log_debug_errno(errno, "Failed to take opened tmp file's fd: %m");

        r = clean_pool_read_first_entry(file, child_error, error);
        if (r < 0)
                return r;

        r = sd_bus_message_new_method_return(message, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "(st)");
        if (r < 0)
                return r;

        /* On success the resulting temporary file will contain a list of image names that were removed followed by
         * their size on disk. Let's read that and turn it into a bus message. */
        for (;;) {
                _cleanup_free_ char *name = NULL;
                uint64_t usage;

                r = clean_pool_read_next_entry(file, &name, &usage);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                r = sd_bus_message_append(reply, "(st)", name, usage);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return sd_bus_message_send(reply);
}
