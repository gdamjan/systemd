/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <unistd.h>

#include "alloc-util.h"
#include "discover-image.h"
#include "fd-util.h"
#include "hashmap.h"
#include "image.h"
#include "log.h"
#include "machined.h"
#include "operation.h"
#include "process-util.h"
#include "set.h"
#include "tmpfile-util.h"

int image_clean_pool_operation(Manager *manager, ImageCleanPoolMode mode, Operation **ret_operation) {
        _cleanup_close_pair_ int errno_pipe_fd[2] = EBADF_PAIR;
        _cleanup_close_ int result_fd = -EBADF;
        _cleanup_(pidref_done_sigkill_wait) PidRef child = PIDREF_NULL;
        int r;

        assert(manager);
        assert(ret_operation);

        if (pipe2(errno_pipe_fd, O_CLOEXEC|O_NONBLOCK) < 0)
                return log_debug_errno(errno, "Failed to create pipe: %m");

        /* Create a temporary file we can dump information about deleted images into. We use a temporary file for this
         * instead of a pipe or so, since this might grow quit large in theory and we don't want to process this
         * continuously */
        result_fd = open_tmpfile_unlinkable(NULL, O_RDWR|O_CLOEXEC);
        if (result_fd < 0)
                return log_debug_errno(result_fd, "Failed to open tmpfile: %m");

        /* This might be a slow operation, run it asynchronously in a background process */
        r = pidref_safe_fork("(sd-clean)", FORK_RESET_SIGNALS, &child);
        if (r < 0)
                return log_debug_errno(r, "Failed to fork(): %m");
        if (r == 0) {
                errno_pipe_fd[0] = safe_close(errno_pipe_fd[0]);

                _cleanup_hashmap_free_ Hashmap *images = NULL;
                r = image_discover(manager->runtime_scope, IMAGE_MACHINE, /* root= */ NULL, &images);
                if (r < 0) {
                        log_debug_errno(r, "Failed to discover images: %m");
                        report_errno_and_exit(errno_pipe_fd[1], r);
                }

                _cleanup_set_free_ Set *mstack_layers = NULL;
                if (mode == IMAGE_CLEAN_POOL_REMOVE_HIDDEN) {
                        r = image_collect_mstack_layers(images, &mstack_layers);
                        if (r < 0)
                                report_errno_and_exit(errno_pipe_fd[1], r);
                }

                _cleanup_free_ Image **remove = NULL;
                size_t n_remove = 0;
                Image *image;
                HASHMAP_FOREACH(image, images) {
                        /* We can't remove vendor images (i.e. those in /usr) */
                        if (image_is_vendor(image))
                                continue;
                        if (image_is_host(image))
                                continue;
                        if (mode == IMAGE_CLEAN_POOL_REMOVE_HIDDEN) {
                                if (!image_is_hidden(image))
                                        continue;
                                if (image_is_mstack_layer(image, mstack_layers)) {
                                        log_debug("Keeping image '%s' because it's referenced by an mstack.", image->name);
                                        continue;
                                }
                        }

                        if (!GREEDY_REALLOC(remove, n_remove + 1)) {
                                log_oom_debug();
                                report_errno_and_exit(errno_pipe_fd[1], -ENOMEM);
                        }

                        remove[n_remove++] = image;
                }

                r = image_clean_pool_remove(remove, n_remove, manager->runtime_scope, result_fd);
                if (r < 0)
                        report_errno_and_exit(errno_pipe_fd[1], r);

                result_fd = safe_close(result_fd);
                _exit(EXIT_SUCCESS);
        }

        errno_pipe_fd[1] = safe_close(errno_pipe_fd[1]);

        /* The clean-up might take a while, hence install a watch on the child and return */
        r = operation_new(manager, /* machine= */ NULL, &child, errno_pipe_fd[0], ret_operation);
        if (r < 0)
                 return r;

        (*ret_operation)->extra_fd = TAKE_FD(result_fd);
        TAKE_FD(errno_pipe_fd[0]);
        TAKE_PIDREF(child);
        return 0;
}
